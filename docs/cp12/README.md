# CP12 — Proposta de upgrade do projeto N1 (Equipe 05 · Access)

Entrega parcial de 20/10/2026 (seções 1 a 4) e entrega completa de 27/10/2026 (seções 1 a 9 e anexos, com o contrato congelado como v1).

| Arquivo | O que é |
| :--- | :--- |
| [`CP12_EQUIPE-05_PROJETO_v1.0.pdf`](CP12_EQUIPE-05_PROJETO_v1.0.pdf) | **PDF completo** (27/10/2026), entrega final no Teams |
| [`CP12_PROJETO_v1.0.md`](CP12_PROJETO_v1.0.md) | Fonte em Markdown do PDF completo |
| [`CP12_EQUIPE-05_PARCIAL.pdf`](CP12_EQUIPE-05_PARCIAL.pdf) | PDF parcial entregue em 20/10 (histórico) |
| [`CP12_PARCIAL.md`](CP12_PARCIAL.md) | Fonte em Markdown do PDF parcial (histórico) |
| [`diagramas/arquitetura-alvo.svg`](diagramas/arquitetura-alvo.svg) | Diagrama de blocos da arquitetura-alvo |
| [`diagramas/fluxo-dado.svg`](diagramas/fluxo-dado.svg) | Fluxo do dado: sensor → firmware → MQTT → backend → banco → interface |
| [`diagramas/fluxo-comando.svg`](diagramas/fluxo-comando.svg) | Fluxo do comando: interface → backend → MQTT → firmware → atuador → confirmação |
| [`diagramas/fluxograma-firmware.svg`](diagramas/fluxograma-firmware.svg) | Fluxograma do firmware: modos, estados do alarme e intertravamentos |
| [`diagramas/esboco-telas.svg`](diagramas/esboco-telas.svg) | Esboço das telas e dos estados do comando |

Fora desta pasta: [`../../CONTRATOS.md`](../../CONTRATOS.md) (contrato MQTT v1) e [`../../hardware/`](../../hardware/README.md) (esquemas, mapa de pinos e BOM).

O PDF é a proposta; o repositório é a prova. Qualquer mudança de rumo nas próximas aulas entra aqui com nova versão do documento.
