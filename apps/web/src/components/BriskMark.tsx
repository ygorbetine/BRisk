/** Marca do BRisk: "B" branco sobre degradê magenta/violeta, com ponto de transmissão ao vivo. */
export function BriskMark({ size = 24 }: { size?: number }) {
  return (
    <svg className="brisk-mark" width={size} height={size} viewBox="0 0 256 256" aria-hidden="true" focusable="false">
      <defs>
        <linearGradient id="brisk-mark-gradient" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="#c026d3" />
          <stop offset="0.55" stopColor="#7c4dff" />
          <stop offset="1" stopColor="#5b6cf9" />
        </linearGradient>
      </defs>
      <rect width="256" height="256" rx="58" fill="url(#brisk-mark-gradient)" />
      <path
        fill="#fff"
        fillRule="evenodd"
        d="M92 64h60c26 0 42 14 42 36 0 14-8 24-20 29 16 4 26 16 26 32 0 24-18 39-46 39H92zM120 88v34h28c12 0 19-6 19-17s-7-17-19-17zM120 146v36h32c13 0 21-7 21-18s-8-18-21-18z"
      />
      <circle cx="206" cy="58" r="11" fill="#ffd6ff" />
    </svg>
  );
}
