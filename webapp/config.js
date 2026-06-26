// Public webapp configuration.
// GitHub Actions overwrites this file in the Pages artifact from public
// repository variables. Do not put secrets here.
window.FB_WEBAPP_CONFIG = {
  signalingUrl: "",
  turnstileSiteKey: "",
  // Leave false unless the Worker is configured with REQUIRE_TURNSTILE_FOR_WEBSOCKET=true.
  turnstileRequiredForWebSocket: false,
  // Public site URL used in copied share links. Defaults to the GitHub Pages URL when empty.
  siteUrl: "",
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
