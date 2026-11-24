"use strict";
// Painel do Access. Fala só com a API do backend, nunca com o broker.

const $ = (seletor) => document.querySelector(seletor);

const SENSOR = {
  abertura: { nome: "Porta", valores: ["Fechada", "Aberta"], eventos: "Aberturas" },
  presenca: { nome: "Presença", valores: ["Nenhuma", "Detectada"], eventos: "Detecções" },
};
const ACAO = { armar: "Armar", desarmar: "Desarmar", disparar: "Disparar sirene", silenciar: "Silenciar sirene",
  configurar: "Configurar", atualizar_firmware: "Atualizar firmware" };
const PARAMETRO = { tempoEntrada_s: "tempo de entrada", intervaloTelemetria_s: "intervalo de envio", timeoutManual_s: "tempo do modo manual" };
const ALERTA = {
  abertura_armado: ["Porta aberta com o sistema armado", ""],
  dispositivo_offline: ["Sem contato com o sistema armado", "leve"],
  falha_sensor: ["Falha no sensor", ""],
  agendamento_nao_aplicado: ["Agendamento não aplicado", "leve"],
};
// Situação de um comando (R7). Os estados intermediários aparecem ao vivo enquanto o dispositivo responde.
const SITUACAO_CMD = {
  enviado: ["Aguardando o dispositivo", "fraco"], recebido: ["Recebido", ""], executado: ["Executado", ""],
  confirmado: ["Confirmado", "ok"], rejeitado: ["Recusado", "alerta"], falha: ["Falhou", "alerta"],
  sem_resposta: ["Sem resposta", "aviso"],
};
const SENSOR_ESTADO = { ok: ["OK", "ok"], calibrando: ["Calibrando", "aviso"],
  falha_aberto: ["Fio rompido", "alerta"], falha_curto: ["Curto-circuito", "alerta"] };
const DIAS = ["Seg", "Ter", "Qua", "Qui", "Sex", "Sáb", "Dom"];

let sessao = null;
let dispositivos = [];
const anterior = {};
const graficos = {};

// --- utilidades -----------------------------------------------------------------
function el(tag, attrs = {}, ...filhos) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") n.className = v;
    else if (k.startsWith("on")) n.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) n.setAttribute(k, v === true ? "" : v);
  }
  for (const f of filhos.flat()) if (f != null && f !== false) n.append(f.nodeType ? f : String(f));
  return n;
}
const span = (texto, classe = "") => el("span", { class: classe }, texto);
const numero = (v, casas = 0) => v.toLocaleString("pt-BR", { minimumFractionDigits: casas, maximumFractionDigits: casas });
const maiuscula = (t) => t.charAt(0).toUpperCase() + t.slice(1);
function quando(ts) {
  const d = new Date(ts);
  const h = d.toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  return d.toDateString() === new Date().toDateString() ? h : `${d.toLocaleDateString("pt-BR", { day: "2-digit", month: "2-digit" })} ${h}`;
}
function ha(ts) {
  if (!ts) return "nunca";
  const s = Math.max(0, Math.round((Date.now() - new Date(ts)) / 1000));
  if (s < 5) return "agora";
  if (s < 60) return `há ${s} s`;
  if (s < 3600) return `há ${Math.floor(s / 60)} min`;
  if (s < 86400) return `há ${Math.floor(s / 3600)} h`;
  return quando(ts);
}
function duracao(s) {
  if (s < 60) return `${s} s`;
  if (s < 3600) return `${Math.floor(s / 60)} min ${s % 60} s`;
  return `${Math.floor(s / 3600)} h ${Math.floor((s % 3600) / 60)} min`;
}
function diasSemana(dias) {
  const k = dias.join();
  if (k === "0,1,2,3,4,5,6") return "Todos os dias";
  if (k === "0,1,2,3,4") return "De segunda a sexta";
  if (k === "5,6") return "Fins de semana";
  return dias.map((i) => DIAS[i]).join(", ");
}
function avisar(texto) {
  const a = $("#aviso");
  a.textContent = texto;
  a.classList.add("visivel");
  clearTimeout(avisar.t);
  avisar.t = setTimeout(() => a.classList.remove("visivel"), 4000);
}
// Só redesenha quando o dado muda: não perde o foco nem o clique a cada atualização.
function mudou(chave, dado) {
  const s = JSON.stringify(dado);
  if (anterior[chave] === s) return false;
  anterior[chave] = s;
  return true;
}
const nomeDe = (id) => dispositivos.find((d) => d.device_id === id)?.nome || id;

async function api(caminho, opcoes = {}) {
  const r = await fetch(`/api${caminho}`, {
    ...opcoes,
    headers: { "Content-Type": "application/json", ...(sessao ? { Authorization: `Bearer ${sessao.token}` } : {}) },
  });
  if (r.status === 401 && sessao) { sair(); throw new Error("sessão expirada"); }
  const corpo = r.status === 204 ? null : await r.json().catch(() => null);
  if (!r.ok) {
    const d = corpo && (corpo.motivo || corpo.detail);
    throw new Error(Array.isArray(d) ? d.map((x) => x.msg).join("; ") : d || `erro ${r.status}`);
  }
  return corpo;
}

// --- sessão ----------------------------------------------------------------------
function guardarSessao(s) {
  try { s ? sessionStorage.setItem("access", JSON.stringify(s)) : sessionStorage.removeItem("access"); } catch { /* sem storage */ }
}
function lerSessao() {
  try { return JSON.parse(sessionStorage.getItem("access")); } catch { return null; }
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
    iniciar();
  } catch (err) {
    $("#erro-login").textContent = err.message === "usuário ou senha inválidos" ? "Usuário ou senha incorretos." : err.message;
  }
});
$("#sair").addEventListener("click", sair);

// --- dispositivos ----------------------------------------------------------------
function grandezaPrincipal(d) {
  const m = (d.ultimas || []).find((x) => SENSOR[x.grandeza]);
  return m ? m.grandeza : d.device_id.startsWith("porta") ? "abertura" : "presenca";
}

// Uma frase sobre o dispositivo, na ordem do que mais importa para quem olha.
function situacao(d) {
  const e = d.estado;
  if (!d.ultimo_contato) return ["Aguardando conexão", "apagado", "O dispositivo ainda não se conectou ao servidor."];
  if (!d.online) return ["Sem contato", "apagado", `Última comunicação ${ha(d.ultimo_contato)}.`];
  if (!e) return ["Aguardando dados", "apagado", "O dispositivo ainda não informou o estado."];
  if (e.alarme === "disparado") return ["Alarme disparado", "alerta", "A sirene está tocando."];
  if (e.alarme === "entrada") return ["Abertura detectada", "aviso", "A sirene toca se o sistema não for desarmado."];
  if (e.sensor?.startsWith("falha")) return ["Falha no sensor", "alerta", "Modo seguro até o sensor voltar ao normal."];
  if (e.sensor === "calibrando") return ["Calibrando sensor", "aviso", "Leva cerca de um minuto depois de ligar."];
  return e.armado ? ["Protegido", "ok", "Sistema armado."] : ["Desarmado", "", "A sirene não toca sozinha."];
}

function blocoDispositivo(d) {
  const e = d.estado || {};
  const medida = Object.fromEntries((d.ultimas || []).map((m) => [m.grandeza, m]));
  const g = grandezaPrincipal(d);
  const leitura = medida[g];
  const [titulo, cor, frase] = situacao(d);
  const sensor = SENSOR_ESTADO[e.sensor];
  const botao = (acao, classe = "") => el("button", { class: classe, disabled: !d.online, onclick: () => comandar(d.device_id, acao) }, ACAO[acao]);
  const linha = (rotulo, ...valor) => el("tr", {}, el("th", {}, rotulo), el("td", {}, ...valor));
  const nada = () => span("—", "apagado");

  return el("article", { class: `dispositivo${e.alarme === "disparado" ? " disparado" : ""}` },
    el("div", { class: "disp-cab" },
      el("div", {}, el("div", { class: "nome" }, d.nome), el("div", { class: "num fraco" }, d.device_id)),
      el("div", { class: "conexao" },
        el("div", { class: d.online ? "ok" : "apagado" }, d.online ? "Online" : "Offline"),
        d.ultimo_contato && el("div", { class: "fraco", "data-ts": d.ultimo_contato }, ha(d.ultimo_contato)))),
    el("div", {}, el("div", { class: `situacao ${cor}` }, titulo), el("p", { class: "fraco" }, frase)),
    el("table", {}, el("tbody", {},
      linha(SENSOR[g].nome, leitura ? span(SENSOR[g].valores[leitura.valor], leitura.valor ? "aviso" : "") : nada()),
      linha("Sensor", sensor ? span(sensor[0], sensor[1]) : nada()),
      linha("Sinal Wi-Fi", medida.rssi ? span(`${medida.rssi.valor} dBm`, "num") : nada()),
      linha(SENSOR[g].eventos, medida.aberturas ? [span(medida.aberturas.valor, "num"), span(" desde que ligou", "fraco")] : nada()),
      e.modo && e.modo !== "automatico" ? linha("Modo", e.modo === "seguro" ? span("Seguro", "alerta") : "Manual") : null,
      linha("Firmware", d.fw_versao ? span(d.fw_versao, "num") : nada()))),
    sessao.perfil === "administrador" && el("div", { class: "acoes" },
      e.armado ? botao("desarmar", "primario") : botao("armar", "primario"),
      e.alarme && e.alarme !== "inativo" ? botao("silenciar") : null,
      botao("disparar", "perigo")));
}

async function comandar(deviceId, acao, params = {}) {
  try {
    const c = await api("/comandos", { method: "POST", body: JSON.stringify({ deviceId, acao, params }) });
    avisar(c.estado === "sem_resposta" ? "Não foi possível enviar: o servidor está sem conexão com o broker."
      : `${descreverAcao(c)}: enviado para ${nomeDe(deviceId)}, aguardando confirmação.`);
    atualizar();
  } catch (err) {
    avisar(`Não enviado: ${err.message}`);
  }
}

// --- alertas, comandos e faixas ----------------------------------------------------
function renderFaixas() {
  const disparados = dispositivos.filter((d) => d.estado?.alarme === "disparado").map((d) => d.nome);
  $("#faixas").replaceChildren(...(disparados.length ? [el("div", {}, `Alarme disparado em ${disparados.join(" e ")}`)] : []));
}

function renderAlertas(lista) {
  $("#secao-alertas").hidden = !lista.length;
  $("#alertas").replaceChildren(...lista.map((a) => {
    const [texto, nivel] = ALERTA[a.tipo];
    return el("div", { class: `alerta-item ${nivel}` },
      el("div", {}, el("b", {}, texto), span(` em ${nomeDe(a.device_id)}`, "fraco")),
      el("div", { class: "fraco" }, `Desde ${quando(a.aberto_em)}`, a.notificado_em ? ", aviso enviado ao celular" : ""));
  }));
}

function descreverAcao(c) {
  if (c.acao === "configurar") {
    return maiuscula(Object.entries(c.params || {}).map(([k, v]) => `${PARAMETRO[k] || k}: ${v} s`).join(", "));
  }
  if (c.acao === "atualizar_firmware") return `Atualizar firmware para ${c.params?.versao}`;
  return ACAO[c.acao] || c.acao;
}

function detalheComando(c) {
  if (c.erro) return [c.motivo ? `${maiuscula(c.motivo)} ` : "", span(`(${c.erro})`, "num apagado")];
  if (c.estado === "sem_resposta") return (c.motivo || "").startsWith("não publicado")
    ? "Servidor sem conexão com o broker." : "O dispositivo não respondeu em 10 s.";
  if (c.estado === "confirmado") {
    const s = (new Date(c.confirmado_em) - new Date(c.enviado_em)) / 1000;
    return s < 0.1 ? "Em menos de 0,1 s" : `Em ${numero(s, 1)} s`;
  }
  return "";
}

function renderComandos(lista) {
  const origem = (c) => c.origem === "usuario" ? (c.nome_usuario || "Usuário") : c.origem === "regra" ? "Regra automática" : "Agendamento";
  $("#comandos").replaceChildren(...lista.map((c) => el("tr", {},
    el("td", { class: "num c-quando" }, quando(c.enviado_em)),
    el("td", { class: "c-disp" }, nomeDe(c.device_id)),
    el("td", { class: "c-acao" }, descreverAcao(c)),
    el("td", { class: "c-origem" }, origem(c)),
    el("td", { class: "c-situacao" }, span(...SITUACAO_CMD[c.estado])),
    el("td", { class: "detalhe c-detalhe" }, detalheComando(c)))));
  if (!lista.length) $("#comandos").append(el("tr", {}, el("td", { colspan: 6, class: "vazio" }, "Nenhum comando enviado ainda.")));
}

async function atualizar() {
  try {
    const [disp, cmds, alertas] = await Promise.all([api("/dispositivos"), api("/comandos?limite=8"), api("/alertas?ativos=true")]);
    dispositivos = disp;
    if (mudou("disp", disp.map(({ ultimo_contato, ...resto }) => resto))) {
      $("#dispositivos").replaceChildren(...disp.map(blocoDispositivo));
      renderFaixas();
      preencherSeletores();
    }
    if (mudou("cmds", cmds)) renderComandos(cmds);
    if (mudou("alertas", alertas)) renderAlertas(alertas);
  } catch (err) {
    avisar(`Sem conexão com o servidor: ${err.message}`);
  }
}

function preencherSeletores() {
  for (const sel of [$("#sel-disp"), ...document.querySelectorAll(".sel-dispositivos")]) {
    const atual = sel.value;
    sel.replaceChildren(...dispositivos.map((d) => el("option", { value: d.device_id }, d.nome)));
    if (atual) sel.value = atual;
  }
  tempoEntradaAtual();
}

// O campo mostra o tempo de entrada que o dispositivo informou no último estado.
function tempoEntradaAtual() {
  const f = $("#form-entrada");
  if (document.activeElement === f.tempo) return;
  const d = dispositivos.find((x) => x.device_id === f.device.value);
  f.tempo.value = d?.estado?.config?.tempoEntrada_s ?? "";
  f.tempo.placeholder = d?.estado ? "" : "o dispositivo ainda não informou";
}
$("#form-entrada").device.addEventListener("change", tempoEntradaAtual);

// --- histórico --------------------------------------------------------------------
function grafico(id, tipo, dados, eixoY = {}, vazio = "") {
  graficos[id]?.destroy();
  delete graficos[id];
  const caixa = $(id).parentElement;
  caixa.querySelector(".sem-dados")?.remove();
  $(id).hidden = Boolean(vazio);
  if (vazio) return caixa.append(el("div", { class: "sem-dados" }, vazio));
  const eixo = { ticks: { color: "#8a8a8a", font: { family: "ui-monospace, Menlo, Consolas, monospace", size: 11 } },
    grid: { color: "#1f1f1f" }, border: { color: "#333333" } };
  graficos[id] = new Chart($(id), {
    type: tipo, data: dados,
    options: {
      responsive: true, maintainAspectRatio: false, animation: false,
      interaction: { mode: "index", intersect: false },
      plugins: { legend: { display: false }, tooltip: { displayColors: false, backgroundColor: "#1f1f1f", borderColor: "#444", borderWidth: 1 } },
      scales: { x: { ...eixo, ticks: { ...eixo.ticks, maxTicksLimit: innerWidth < 640 ? 4 : 6, maxRotation: 0 } }, y: { ...eixo, ...eixoY } },
    },
  });
}

function mudancas(leituras) {
  const saida = [];
  for (const m of leituras) if (!saida.length || saida.at(-1).valor !== m.valor) saida.push(m);
  return saida.slice(1).reverse();   // a primeira leitura do período não é mudança
}

const indicador = (valor, rotulo) => el("div", { class: "indicador" }, el("b", {}, valor), span(rotulo));

async function historico() {
  const id = $("#sel-disp").value;
  if (!id || !window.Chart) return;
  const horas = Number($("#sel-horas").value);
  const g = grandezaPrincipal(dispositivos.find((d) => d.device_id === id) || { device_id: id });
  const de = new Date(Date.now() - horas * 3600e3).toISOString();
  try {
    const [a, leituras] = await Promise.all([api(`/analitico/${id}?horas=${horas}`),
      api(`/dispositivos/${id}/medicoes?grandeza=${g}&de=${encodeURIComponent(de)}&limite=2000`)]);
    if (!mudou("historico", [id, horas, a, leituras.length && leituras.at(-1)])) return;

    const total = a.eventos_por_hora.reduce((s, h) => s + h.eventos, 0);
    $("#indicadores").replaceChildren(
      indicador(numero(total), SENSOR[g].eventos),
      indicador(duracao(a.tempo_em_alarme_s), "Com alarme disparado"),
      indicador(a.uptime_pct || dispositivos.find((d) => d.device_id === id)?.ultimo_contato ? `${numero(a.uptime_pct, 1)} %` : "—", "Do tempo online"),
      indicador(a.rssi.length ? `${numero(a.rssi.at(-1).media, 1)} dBm` : "—", "Sinal médio recente"));

    const porHora = a.eventos_por_hora.slice(-24);
    $("#tit-eventos").textContent = `${SENSOR[g].eventos} por hora`;
    grafico("#g-eventos", "bar", {
      labels: porHora.map((h) => `${String(new Date(h.hora).getHours()).padStart(2, "0")}h`),
      datasets: [{ label: SENSOR[g].eventos, data: porHora.map((h) => h.eventos), backgroundColor: "#5a5a5a", hoverBackgroundColor: "#d4d4d4", borderRadius: 2 }],
    }, { beginAtZero: true, ticks: { precision: 0, color: "#8a8a8a" } }, total ? "" : `Nenhuma ${SENSOR[g].nome === "Porta" ? "abertura" : "detecção"} no período.`);
    grafico("#g-rssi", "line", {
      labels: a.rssi.map((p) => new Date(p.ts).toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit" })),
      datasets: [
        { label: "Leitura", data: a.rssi.map((p) => p.valor), borderColor: "#4a4a4a", borderWidth: 1, pointRadius: 0 },
        { label: "Média", data: a.rssi.map((p) => p.media), borderColor: "#7ec699", borderWidth: 1.75, pointRadius: 0 },
      ],
    }, {}, a.rssi.length ? "" : "Sem leituras de sinal ainda.");

    const ultimas = mudancas(leituras).slice(0, 7);
    $("#tit-ultimos").textContent = `Últimas mudanças: ${SENSOR[g].nome.toLowerCase()}`;
    $("#ultimos").replaceChildren(...(ultimas.length
      ? ultimas.map((m) => el("tr", {}, el("td", { class: "num fraco" }, quando(m.ts)),
          el("td", { class: m.valor ? "aviso" : "" }, SENSOR[g].valores[m.valor])))
      : [el("tr", {}, el("td", { class: "vazio" }, "Nenhuma mudança no período."))]));
  } catch (err) {
    avisar(`Erro ao carregar o histórico: ${err.message}`);
  }
}

// --- mensagens recusadas ------------------------------------------------------------
async function invalidas() {
  if (!$("#registro").open) return;
  const lista = await api("/mensagens-invalidas?limite=20").catch(() => []);
  if (!mudou("invalidas", lista)) return;
  $("#invalidas").replaceChildren(...(lista.length
    ? lista.map((m) => el("tr", {},
        el("td", { "data-rotulo": "Quando", class: "num" }, quando(m.recebido_em)),
        el("td", { "data-rotulo": "Tópico", class: "num" }, m.topico),
        el("td", { "data-rotulo": "Motivo", class: "detalhe" }, m.motivo)))
    : [el("tr", {}, el("td", { colspan: 3, class: "vazio" }, "Nenhuma mensagem recusada."))]));
}
$("#registro").addEventListener("toggle", invalidas);

// --- configurações (administrador) -------------------------------------------------------
async function configuracoes() {
  const [cfg, ags] = await Promise.all([api("/configuracao"), api("/agendamentos")]);
  $("#form-limite").limite.value = cfg.limiteEscalonamento_s;
  $("#agendamentos").replaceChildren(...(ags.length
    ? ags.map((a) => el("div", { class: "agendamento" },
        el("div", {}, el("div", {}, `${ACAO[a.acao]} às `, span(a.hora.slice(0, 5), "num")),
          el("div", { class: "fraco" }, `${nomeDe(a.device_id)}, ${diasSemana(a.dias_semana).toLowerCase()}`)),
        el("button", { class: "link", type: "button", onclick: () => removerAgendamento(a.id) }, "Remover")))
    : [el("div", { class: "vazio" }, "Nenhum agendamento.")]));
}
async function removerAgendamento(id) {
  try { await api(`/agendamentos/${id}`, { method: "DELETE" }); configuracoes(); avisar("Agendamento removido."); }
  catch (err) { avisar(`Erro: ${err.message}`); }
}

$("#form-limite").addEventListener("submit", async (e) => {
  e.preventDefault();
  try {
    await api("/configuracao", { method: "PUT", body: JSON.stringify({ limiteEscalonamento_s: Number(e.target.limite.value) }) });
    avisar("Limite salvo.");
  } catch (err) { avisar(`Erro: ${err.message}`); }
});
$("#form-entrada").addEventListener("submit", (e) => {
  e.preventDefault();
  comandar(e.target.device.value, "configurar", { tempoEntrada_s: Number(e.target.tempo.value) });
  e.target.tempo.blur();
});
$("#form-agenda").addEventListener("submit", async (e) => {
  e.preventDefault();
  const f = e.target;
  const dias = [...f.querySelectorAll("input[name=dia]:checked")].map((c) => Number(c.value));
  if (!dias.length) return avisar("Escolha ao menos um dia.");
  try {
    await api("/agendamentos", { method: "POST", body: JSON.stringify({ device_id: f.device.value, acao: f.acao.value, hora: f.hora.value, dias_semana: dias }) });
    avisar("Agendamento criado.");
    f.hora.value = "";
    configuracoes();
  } catch (err) { avisar(`Erro: ${err.message}`); }
});

// --- início ---------------------------------------------------------------------------
async function iniciar() {
  $("#tela-login").hidden = true;
  $("#tela-painel").hidden = false;
  $("#usuario").textContent = sessao.perfil === "administrador" ? `${sessao.nome}, administrador` : sessao.nome;
  document.body.classList.toggle("admin", sessao.perfil === "administrador");
  $("#dias").replaceChildren(...DIAS.map((d, i) => el("label", {}, el("input", { type: "checkbox", name: "dia", value: i, checked: i < 5 }), d)));
  await atualizar();
  historico();
  if (sessao.perfil === "administrador") configuracoes().catch((err) => avisar(err.message));
  setInterval(atualizar, 2000);
  setInterval(() => { historico(); invalidas(); }, 15000);
  setInterval(() => document.querySelectorAll("[data-ts]").forEach((s) => { s.textContent = ha(s.dataset.ts || null); }), 1000);
  $("#sel-disp").addEventListener("change", historico);
  $("#sel-horas").addEventListener("change", historico);
}

sessao = lerSessao();
if (sessao) {
  api("/eu").then(iniciar).catch(() => { sessao = null; guardarSessao(null); $("#tela-login").hidden = false; });
} else {
  $("#tela-login").hidden = false;
}
