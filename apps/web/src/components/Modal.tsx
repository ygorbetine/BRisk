import React, { useEffect, useId, useRef } from "react";

export function Modal({ title, children, onClose }: { title: string; children: React.ReactNode; onClose(): void }) {
  const titleId = useId();
  const dialog = useRef<HTMLElement>(null);
  const previousFocus = useRef<HTMLElement | null>(null);
  // O pai costuma passar um onClose novo a cada renderização. Se ele fosse dependência do
  // efeito, cada re-render da tela devolveria o foco ao botão de origem e tiraria o cursor do campo.
  const closeRef = useRef(onClose);
  closeRef.current = onClose;
  useEffect(() => {
    previousFocus.current = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    const first = dialog.current?.querySelector<HTMLElement>("input:not(:disabled),select:not(:disabled),textarea:not(:disabled)")
      ?? dialog.current?.querySelector<HTMLElement>("button,[tabindex]:not([tabindex='-1'])");
    first?.focus();
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") { event.preventDefault(); closeRef.current(); return; }
      if (event.key !== "Tab" || !dialog.current) return;
      const focusable = [...dialog.current.querySelectorAll<HTMLElement>("button:not(:disabled),input:not(:disabled),select:not(:disabled),textarea:not(:disabled),[tabindex]:not([tabindex='-1'])")];
      if (!focusable.length) return;
      const firstItem = focusable[0]!; const lastItem = focusable.at(-1)!;
      if (event.shiftKey && document.activeElement === firstItem) { event.preventDefault(); lastItem.focus(); }
      else if (!event.shiftKey && document.activeElement === lastItem) { event.preventDefault(); firstItem.focus(); }
    };
    document.addEventListener("keydown", onKeyDown);
    return () => { document.removeEventListener("keydown", onKeyDown); previousFocus.current?.focus(); };
  }, []);
  return <div className="modal-backdrop" onMouseDown={onClose}>
    <section ref={dialog} className="modal" role="dialog" aria-modal="true" aria-labelledby={titleId} onMouseDown={(event) => event.stopPropagation()}>
      <header className="modal-header">
        <h2 id={titleId}>{title}</h2>
        <button className="modal-close" aria-label="Fechar" onClick={onClose}>×</button>
      </header>
      <div className="modal-content">{children}</div>
    </section>
  </div>;
}
