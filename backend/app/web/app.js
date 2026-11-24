"use strict";
// Interface do Access: fala só com a API do backend (nunca com o broker).

const $ = (sel) => document.querySelector(sel);
const ROTULOS = {
  abertura: { nome: "Porta", valores: ["Fechada", "Aberta"] },
  presenca: { nome: "Presença", valores: ["Nenhuma", "Detectada"] },
};
const ACOES = { armar: "Armar", desarmar: "Desarmar", disparar: "Disparar", silenciar: "Silenciar",
  configurar: "Configurar", atualizar_firmware: "Atualizar firmware" };
const ORIGENS = { usuario: "Usuário", regra: "Regra", agendamento: "Agendamento" };
const ALERTAS = { abertura_armado: "Invasão: aberta com o sistema armado", dispositivo_offline: "Dispositivo armado sem contato",
  falha_sensor: "Falha de sensor", agendamento_nao_aplicado: "Agendamento não aplicado" };
const DIAS = ["Seg", "Ter", "Qua", "Qui", "Sex", "Sáb", "Dom"];

let sessao = null;
let ultimoDisp = "", ultimoCmd = "", ultimoAlerta = "";
let dispositivos = [];
const graficos = {};

// --- utilidades -------------------------------------------------------------
function el(tag, attrs = {}, ...filhos) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") n.className = v;
    else if (k.startsWith("on")) n.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) n.setAttribute(k, v === true ? "" : v);
  }
  for (const f of filhos.flat()) if (f != null) n.append(f.nodeType ? f : String(f));
  return n;
}
function icone(nome) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  const use = document.createElementNS("http://www.w3.org/2000/svg", "use");
  use.setAttribute("href", `#i-${nome}`);
  svg.append(use);
  return svg;
}
const hora = (ts) => new Date(ts).toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
const dataHora = (ts) => new Date(ts).toLocaleString("pt-BR", { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit" });
function ha(ts) {
  if (!ts) return "nunca";
  const s = Math.max(0, Math.round((Date.now() - new Date(ts)) / 1000));
  if (s < 60) return `há ${s} s`;
  if (s < 3600) return `há ${Math.floor(s / 60)} min`;
  return dataHora(ts);
}
function duracao(s) {
  if (s < 60) return `${s} s`;
  if (s < 3600) return `${Math.floor(s / 60)} min ${s % 60} s`;
  return `${Math.floor(s / 3600)} h ${Math.floor((s % 3600) / 60)} min`;
}
function avisar(texto) {
  const a = $("#aviso");
  a.textContent = texto;
  a.classList.add("visivel");
  clearTimeout(avisar.t);
  avisar.t = setTimeout(() => a.classList.remove("visivel"), 3500);
}

async function api(caminho, opcoes = {}) {
  const r = await fetch(`/api${caminho}`, {
    ...opcoes,
    headers: { "Content-Type": "application/json", ...(sessao ? { Authorization: `Bearer ${sessao.token}` } : {}) },
  });
  if (r.status === 401 && sessao) { sair(); throw new Error("Sessão expirada"); }
  const corpo = r.status === 204 ? null : await r.json().catch(() => null);
  if (!r.ok) {
    const d = corpo && (corpo.motivo || corpo.detail);
    throw new Error(Array.isArray(d) ? d.map((x) => x.msg).join("; ") : d || `Erro ${r.status}`);
  }
  return corpo;
}

// --- sessão -------------------------------------------------------------------
function lerSessao() {
  try { return JSON.parse(sessionStorage.getItem("access-sessao")); } catch { return null; }
}
function guardarSessao(s) {
  try { s ? sessionStorage.setItem("access-sessao", JSON.stringify(s)) : sessionStorage.removeItem("access-sessao"); } catch { /* sem storage */ }
}
function sair() {
  sessao = null;
  guardarSessao(null);
  location.reload();
}

$("#form-login").addEventListener("submit", async (e) => {
  e.preventDefault();
  const f = new FormData(e.target);
  try {
    sessao = await api("/auth/login", { method: "POST", body: JSON.stringify({ usuario: f.get("usuario"), senha: f.get("senha") }) });
    guardarSessao(sessao);
    iniciarPainel();
  } catch (err) {
    $("#erro-login").textContent = err.message;
  }
});
$("#sair").addEventListener("click", sair);

// --- dispositivos -------------------------------------------------------------
function valorPrincipal(d) {
  const m = (d.ultimas || []).find((x) => ROTULOS[x.grandeza]);
  if (!m) return el("div", { class: "valor-principal" }, "Sem leitura");
  const r = ROTULOS[m.grandeza];
  return el("div", { class: `valor-principal ${m.valor ? "atencao" : ""}` },
    `${r.nome}: ${r.valores[m.valor]}`, el("small", {}, `(${m.valor} ${m.unidade})`));
}

function metrica(rotulo, valor, classe = "") {
  return el("div", { class: "metrica" }, el("span", {}, rotulo), el("b", { class: classe }, valor));
}

function cartaoDispositivo(d) {
  const e = d.estado || {};
  const medida = Object.fromEntries((d.ultimas || []).map((m) => [m.grandeza, m]));
  const ehPorta = (d.ultimas || []).some((m) => m.grandeza === "abertura") || d.device_id.startsWith("porta");
  const alarme = { inativo: ["Inativo", "ok"], entrada: ["Entrada", "alerta"], disparado: ["Disparado", "perigo"] }[e.alarme] || ["–", ""];
  const sensor = e.sensor ? (e.sensor === "ok" ? ["OK", "ok"] : e.sensor === "calibrando" ? ["Calibrando", "alerta"] : ["Falha", "perigo"]) : ["–", ""];
  const botao = (acao, classe = "") => el("button", { class: classe, disabled: !d.online, onclick: () => comandar(d.device_id, acao) }, ACOES[acao]);
  return el("article", { class: "cartao disp" },
    el("div", { class: "disp-topo" },
      el("div", { class: "disp-icone" }, icone(ehPorta ? "porta" : "radar")),
      el("div", {}, el("div", { class: "disp-nome" }, d.nome), el("div", { class: "disp-id" }, d.device_id)),
      el("span", { class: `pilula ${d.online ? "online" : "offline"}` }, d.online ? "Online" : "Offline")),
    valorPrincipal(d),
    el("div", { class: "metricas" },
      metrica("Sistema", e.armado == null ? "–" : e.armado ? "Armado" : "Desarmado", e.armado ? "ok" : ""),
      metrica("Alarme", alarme[0], alarme[1]),
      metrica("Sensor", sensor[0], sensor[1]),
      metrica("Sinal Wi-Fi", medida.rssi ? `${medida.rssi.valor} dBm` : "–"),
      metrica("Aberturas", medida.aberturas ? `${medida.aberturas.valor}` : "–"),
      metrica("Modo", e.modo || "–", e.modo === "seguro" ? "perigo" : "")),
    sessao.perfil === "administrador"
      ? el("div", { class: "acoes" }, botao("armar", "primario"), botao("desarmar"), botao("silenciar"), botao("disparar", "perigo"))
      : null,
    el("div", { class: "rodape-disp" }, "Último contato ", el("span", { "data-ts": d.ultimo_contato || "" }, ha(d.ultimo_contato)),
      d.fw_versao ? ` · firmware ${d.fw_versao}` : ""));
}

async function comandar(deviceId, acao, params = {}) {
  try {
    const c = await api("/comandos", { method: "POST", body: JSON.stringify({ deviceId, acao, params }) });
    avisar(c.estado === "sem_resposta" ? `Não enviado: ${c.motivo}` : `${ACOES[acao]} enviado a ${deviceId}: aguardando confirmação`);
    atualizar();
  } catch (err) {
    avisar(`Erro: ${err.message}`);
  }
}

// --- comandos e alertas -------------------------------------------------------
const PASSOS = ["enviado", "recebido", "executado", "confirmado"];
const ALCANCE = { enviado: 0, sem_resposta: 0, rejeitado: 0, recebido: 1, falha: 1, executado: 2, confirmado: 3 };
const RUINS = { rejeitado: "rejeitado", falha: "falhou", sem_resposta: "sem resposta" };

function progresso(estado) {
  const passos = PASSOS.map((p, i) => el("span", { class: `passo ${i <= ALCANCE[estado] ? "feito" : ""}` }, p));
  if (RUINS[estado]) passos.splice(ALCANCE[estado] + 1, PASSOS.length, el("span", { class: "passo ruim" }, RUINS[estado]));
  return el("span", { class: "passos" }, passos);
}

function renderComandos(lista) {
  $("#comandos tbody").replaceChildren(...lista.map((c) => el("tr", {},
    el("td", {}, dataHora(c.enviado_em)), el("td", {}, c.device_id),
    el("td", {}, ACOES[c.acao] || c.acao), el("td", {}, ORIGENS[c.origem] + (c.nome_usuario ? ` (${c.nome_usuario})` : "")),
    el("td", {}, progresso(c.estado)),
    el("td", { class: "detalhe" }, c.erro ? `${c.erro}: ${c.motivo || ""}` : c.motivo || ""))));
  if (!lista.length) $("#comandos tbody").append(el("tr", {}, el("td", { colspan: 6, class: "detalhe" }, "Nenhum comando ainda.")));
}

function renderAlertas(lista) {
  $("#alertas").replaceChildren(...lista.map((a) => el("div", { class: `alerta ${a.tipo === "abertura_armado" || a.tipo === "falha_sensor" ? "" : "leve"}` },
    icone("sino"), el("div", {}, el("b", {}, `${ALERTAS[a.tipo]} · ${a.device_id}`), el("div", {}, `${a.detalhe || ""} · desde ${hora(a.aberto_em)}`)))));
}

async function atualizar() {
  try {
    const [disp, cmds, alertas] = await Promise.all([api("/dispositivos"), api("/comandos?limite=15"), api("/alertas?ativos=true")]);
    dispositivos = disp;
    const sd = JSON.stringify(disp.map(({ ultimo_contato, ...resto }) => resto));
    if (sd !== ultimoDisp) {
      ultimoDisp = sd;
      $("#dispositivos").replaceChildren(...disp.map(cartaoDispositivo));
      preencherSeletores();
    }
    const sc = JSON.stringify(cmds);
    if (sc !== ultimoCmd) { ultimoCmd = sc; renderComandos(cmds); }
    const sa = JSON.stringify(alertas);
    if (sa !== ultimoAlerta) { ultimoAlerta = sa; renderAlertas(alertas); }
  } catch (err) {
    avisar(`Sem conexão com o servidor: ${err.message}`);
  }
}

function preencherSeletores() {
  for (const sel of [$("#sel-disp"), ...document.querySelectorAll(".sel-dispositivos")]) {
    const atual = sel.value;
    sel.replaceChildren(...dispositivos.map((d) => el("option", { value: d.device_id }, `${d.nome} (${d.device_id})`)));
    if (atual) sel.value = atual;
  }
}

// --- histórico e análise ------------------------------------------------------------
function grafico(id, config) {
  graficos[id]?.destroy();
  graficos[id] = new Chart($(id), {
    ...config,
    options: { responsive: true, maintainAspectRatio: false, animation: false,
      plugins: { legend: { labels: { color: "#8fa1bd" } } },
      scales: { x: { ticks: { color: "#8fa1bd", maxTicksLimit: 8 }, grid: { color: "#1b2740" } },
                y: { ticks: { color: "#8fa1bd" }, grid: { color: "#1b2740" }, ...(config.y || {}) } } },
  });
}

async function analisar() {
  const id = $("#sel-disp").value;
  if (!id || !window.Chart) return;
  const horas = $("#sel-horas").value;
  const de = new Date(Date.now() - horas * 3600e3).toISOString();
  const d = dispositivos.find((x) => x.device_id === id);
  const grandeza = (d?.ultimas || []).some((m) => m.grandeza === "presenca") ? "presenca" : "abertura";
  try {
    const [a, hist] = await Promise.all([api(`/analitico/${id}?horas=${horas}`),
      api(`/dispositivos/${id}/medicoes?grandeza=${grandeza}&de=${encodeURIComponent(de)}&limite=1000`)]);
    const total = a.eventos_por_hora.reduce((s, h) => s + h.eventos, 0);
    $("#kpi-eventos").textContent = `${total} ${grandeza === "abertura" ? "aberturas" : "detecções"}`;
    $("#kpi-alarme").textContent = duracao(a.tempo_em_alarme_s);
    $("#kpi-uptime").textContent = `${a.uptime_pct}%`;
    $("#kpi-rssi").textContent = a.rssi.length ? `${a.rssi.at(-1).media} dBm` : "–";
    $("#tit-sensor").textContent = `${ROTULOS[grandeza].nome} ao longo do tempo (1 = ${ROTULOS[grandeza].valores[1].toLowerCase()})`;

    grafico("#g-sensor", { type: "line", y: { min: 0, max: 1, ticks: { stepSize: 1, color: "#8fa1bd" } },
      data: { labels: hist.map((m) => hora(m.ts)),
        datasets: [{ label: ROTULOS[grandeza].nome, data: hist.map((m) => m.valor), stepped: true, borderColor: "#14b8a6", pointRadius: 0, fill: true, backgroundColor: "rgba(20,184,166,.15)" }] } });
    const porHora = a.eventos_por_hora.slice(-Math.min(24, a.eventos_por_hora.length));
    grafico("#g-eventos", { type: "bar", y: { beginAtZero: true, ticks: { precision: 0, color: "#8fa1bd" } },
      data: { labels: porHora.map((h) => new Date(h.hora).toLocaleString("pt-BR", { day: "2-digit", hour: "2-digit" }) + "h"),
        datasets: [{ label: "Eventos", data: porHora.map((h) => h.eventos), backgroundColor: "#f59e0b" }] } });
    grafico("#g-rssi", { type: "line",
      data: { labels: a.rssi.map((p) => hora(p.ts)),
        datasets: [{ label: "RSSI", data: a.rssi.map((p) => p.valor), borderColor: "#64748b", pointRadius: 0 },
                   { label: "Média móvel (5)", data: a.rssi.map((p) => p.media), borderColor: "#14b8a6", pointRadius: 0, borderWidth: 2 }] } });
  } catch (err) {
    avisar(`Erro ao carregar análise: ${err.message}`);
  }
}

// --- configurações (administrador) --------------------------------------------------
async function carregarConfiguracoes() {
  const [cfg, ags] = await Promise.all([api("/configuracao"), api("/agendamentos")]);
  $("#form-limite").limite.value = cfg.limiteEscalonamento_s;
  $("#agendamentos").replaceChildren(...ags.map((a) => el("li", {},
    el("span", {}, `${ACOES[a.acao]} ${a.device_id} às ${a.hora.slice(0, 5)} · ${a.dias_semana.map((i) => DIAS[i]).join(", ")}`),
    el("button", { onclick: async () => { await api(`/agendamentos/${a.id}`, { method: "DELETE" }); carregarConfiguracoes(); } }, "Remover"))));
  if (!ags.length) $("#agendamentos").append(el("li", { class: "dica" }, "Nenhum agendamento."));
}

$("#form-limite").addEventListener("submit", async (e) => {
  e.preventDefault();
  try {
    await api("/configuracao", { method: "PUT", body: JSON.stringify({ limiteEscalonamento_s: Number(e.target.limite.value) }) });
    avisar("Limite salvo");
  } catch (err) { avisar(`Erro: ${err.message}`); }
});
$("#form-entrada").addEventListener("submit", (e) => {
  e.preventDefault();
  comandar(e.target.device.value, "configurar", { tempoEntrada_s: Number(e.target.tempo.value) });
});
$("#form-agenda").addEventListener("submit", async (e) => {
  e.preventDefault();
  const f = e.target;
  const dias = [...f.querySelectorAll("input[name=dia]:checked")].map((c) => Number(c.value));
  if (!dias.length) return avisar("Escolha ao menos um dia");
  try {
    await api("/agendamentos", { method: "POST", body: JSON.stringify({ device_id: f.device.value, acao: f.acao.value, hora: f.hora.value, dias_semana: dias }) });
    avisar("Agendamento criado");
    carregarConfiguracoes();
  } catch (err) { avisar(`Erro: ${err.message}`); }
});

// --- início ----------------------------------------------------------------------
async function iniciarPainel() {
  $("#tela-login").hidden = true;
  $("#tela-painel").hidden = false;
  $("#usuario-nome").textContent = sessao.nome;
  $("#usuario-perfil").textContent = sessao.perfil;
  document.body.classList.toggle("admin", sessao.perfil === "administrador");
  $("#dias").replaceChildren(...DIAS.map((d, i) => el("label", {}, el("input", { type: "checkbox", name: "dia", value: i, checked: i < 5 }), d)));
  await atualizar();
  analisar();
  if (sessao.perfil === "administrador") carregarConfiguracoes().catch((err) => avisar(err.message));
  setInterval(atualizar, 2000);
  setInterval(analisar, 15000);
  setInterval(() => document.querySelectorAll("[data-ts]").forEach((s) => { s.textContent = ha(s.dataset.ts || null); }), 1000);
  $("#sel-disp").addEventListener("change", analisar);
  $("#sel-horas").addEventListener("change", analisar);
}

sessao = lerSessao();
if (sessao) {
  api("/eu").then(iniciarPainel).catch(() => { sessao = null; guardarSessao(null); $("#tela-login").hidden = false; });
} else {
  $("#tela-login").hidden = false;
}
