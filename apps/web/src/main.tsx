// A janela de sobreposição (jogos) carrega só o necessário: nada do app, banco local ou tema.
const overlayMode = window.location.hash === "#overlay" ? "overlay" : window.location.hash === "#overlay-demo" ? "demo" : null;

if (overlayMode) {
  void Promise.all([import("react"), import("react-dom/client"), import("./overlay/OverlayApp")]).then(([React, { createRoot }, { OverlayApp }]) => {
    createRoot(document.getElementById("root")!).render(React.createElement(OverlayApp, { demo: overlayMode === "demo" }));
  });
} else {
  void import("./main-app");
}
