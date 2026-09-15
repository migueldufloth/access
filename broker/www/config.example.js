// Copie para "config.js" (mesma pasta) e preencha com os valores reais.
// "config.js" está no .gitignore e nunca deve ser versionado — mesmo assim,
// a senha aqui dentro fica visível no navegador de quem abrir a página
// (ver aviso em index.html). É por isso que o ACL do broker impede o
// usuário "access_web" de publicar telemetria: o pior caso de vazamento
// dessa senha é alguém forjar comando, não forjar sensor.

const ACCESS_CONFIG = {
  mqttUri: "wss://host/caminho",
  usuario: "access_web",
  senha: "senha-do-broker",
};
