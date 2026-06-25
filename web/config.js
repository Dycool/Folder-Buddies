// Public webapp configuration.
// GitHub Actions overwrites this file in the Pages artifact from public
// repository variables. Do not put secrets here.
window.FB_WEBAPP_CONFIG = {
  signalingUrl: "",
  turnstileSiteKey: "",
  // Optional automatic fallback signaling when Cloudflare WebSocket signaling is unavailable.
  // These Firebase web config values are public; protect the database with rules.
  firebase: {
    apiKey: "",
    authDomain: "",
    databaseURL: "",
    projectId: "",
    appId: "",
  },
};
