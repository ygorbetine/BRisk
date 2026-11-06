/** Marca do Briks: um "B" montado com tijolos brancos sobre degradê magenta/violeta. */
export function BriksMark({ size = 24 }: { size?: number }) {
  return (
    <svg className="briks-mark" width={size} height={size} viewBox="0 0 256 256" aria-hidden="true" focusable="false">
      <defs>
        <linearGradient id="briks-mark-gradient" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="#c026d3" />
          <stop offset="0.55" stopColor="#7c4dff" />
          <stop offset="1" stopColor="#5b6cf9" />
        </linearGradient>
      </defs>
      <rect width="256" height="256" rx="58" fill="url(#briks-mark-gradient)" />
      <rect x="63.8" y="30.8" width="40.5" height="18.5" rx="3.5" fill="#ffffff"/>
      <rect x="107.8" y="30.8" width="40.5" height="18.5" rx="3.5" fill="#ecd6ff"/>
      <rect x="151.8" y="30.8" width="18.5" height="18.5" rx="3.5" fill="#ffffff"/>
      <rect x="63.8" y="52.8" width="18.5" height="18.5" rx="3.5" fill="#ffe3fb"/>
      <rect x="85.8" y="52.8" width="18.5" height="18.5" rx="3.5" fill="#ffffff"/>
      <rect x="151.8" y="52.8" width="18.5" height="18.5" rx="3.5" fill="#ffe3fb"/>
      <rect x="173.8" y="52.8" width="18.5" height="18.5" rx="3.5" fill="#ffffff"/>
      <rect x="63.8" y="74.8" width="40.5" height="18.5" rx="3.5" fill="#ecd6ff"/>
      <rect x="151.8" y="74.8" width="40.5" height="18.5" rx="3.5" fill="#ecd6ff"/>
      <rect x="63.8" y="96.8" width="18.5" height="18.5" rx="3.5" fill="#f6e9ff"/>
      <rect x="85.8" y="96.8" width="40.5" height="18.5" rx="3.5" fill="#ecd6ff"/>
      <rect x="129.8" y="96.8" width="40.5" height="18.5" rx="3.5" fill="#ffffff"/>
      <rect x="63.8" y="118.8" width="40.5" height="18.5" rx="3.5" fill="#ffffff"/>
      <rect x="107.8" y="118.8" width="40.5" height="18.5" rx="3.5" fill="#ecd6ff"/>
      <rect x="151.8" y="118.8" width="18.5" height="18.5" rx="3.5" fill="#ffffff"/>
      <rect x="63.8" y="140.8" width="18.5" height="18.5" rx="3.5" fill="#ffe3fb"/>
      <rect x="85.8" y="140.8" width="18.5" height="18.5" rx="3.5" fill="#ffffff"/>
      <rect x="151.8" y="140.8" width="18.5" height="18.5" rx="3.5" fill="#ffe3fb"/>
      <rect x="173.8" y="140.8" width="18.5" height="18.5" rx="3.5" fill="#ffffff"/>
      <rect x="63.8" y="162.8" width="40.5" height="18.5" rx="3.5" fill="#ecd6ff"/>
      <rect x="151.8" y="162.8" width="40.5" height="18.5" rx="3.5" fill="#ecd6ff"/>
      <rect x="63.8" y="184.8" width="18.5" height="18.5" rx="3.5" fill="#f6e9ff"/>
      <rect x="85.8" y="184.8" width="18.5" height="18.5" rx="3.5" fill="#ecd6ff"/>
      <rect x="151.8" y="184.8" width="18.5" height="18.5" rx="3.5" fill="#f6e9ff"/>
      <rect x="173.8" y="184.8" width="18.5" height="18.5" rx="3.5" fill="#ecd6ff"/>
      <rect x="63.8" y="206.8" width="40.5" height="18.5" rx="3.5" fill="#ffffff"/>
      <rect x="107.8" y="206.8" width="40.5" height="18.5" rx="3.5" fill="#ecd6ff"/>
      <rect x="151.8" y="206.8" width="18.5" height="18.5" rx="3.5" fill="#ffffff"/>
    </svg>
  );
}
