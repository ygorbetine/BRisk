export type Rgb = { r: number; g: number; b: number };
export type Hsv = { h: number; s: number; v: number };
export type Point = { x: number; y: number };
export type TriangleVertices = { pure: Point; white: Point; black: Point };

const clamp = (value: number, min: number, max: number) => Math.min(max, Math.max(min, value));

export function normalizeHex(value: string): string | null {
  const match = /^#?([0-9a-f]{6})$/i.exec(value.trim());
  return match ? `#${match[1]!.toLowerCase()}` : null;
}

export function hexToRgb(hex: string): Rgb {
  const normalized = normalizeHex(hex) ?? "#000000";
  return { r: parseInt(normalized.slice(1, 3), 16), g: parseInt(normalized.slice(3, 5), 16), b: parseInt(normalized.slice(5, 7), 16) };
}

export function rgbToHex({ r, g, b }: Rgb): string {
  return `#${[r, g, b].map((channel) => clamp(Math.round(channel), 0, 255).toString(16).padStart(2, "0")).join("")}`;
}

export function rgbToHsv({ r, g, b }: Rgb): Hsv {
  const red = r / 255, green = g / 255, blue = b / 255;
  const max = Math.max(red, green, blue), min = Math.min(red, green, blue), delta = max - min;
  let h = 0;
  if (delta > 0) {
    if (max === red) h = ((green - blue) / delta) % 6;
    else if (max === green) h = (blue - red) / delta + 2;
    else h = (red - green) / delta + 4;
    h *= 60;
    if (h < 0) h += 360;
  }
  return { h, s: max === 0 ? 0 : delta / max, v: max };
}

export function hsvToRgb({ h, s, v }: Hsv): Rgb {
  const hue = ((h % 360) + 360) % 360;
  const c = v * s, x = c * (1 - Math.abs(((hue / 60) % 2) - 1)), m = v - c;
  const [r, g, b] = hue < 60 ? [c, x, 0] : hue < 120 ? [x, c, 0] : hue < 180 ? [0, c, x] : hue < 240 ? [0, x, c] : hue < 300 ? [x, 0, c] : [c, 0, x];
  return { r: (r! + m) * 255, g: (g! + m) * 255, b: (b! + m) * 255 };
}

export const hexToHsv = (hex: string): Hsv => rgbToHsv(hexToRgb(hex));
export const hsvToHex = (hsv: Hsv): string => rgbToHex(hsvToRgb(hsv));

/** Vértices do triângulo S/V (estilo Blender): o triângulo gira com o matiz. Ângulos em graus, y para baixo. */
export function triangleVertices(center: Point, radius: number, hue: number): TriangleVertices {
  const at = (degrees: number): Point => {
    const radians = (degrees * Math.PI) / 180;
    return { x: center.x + radius * Math.cos(radians), y: center.y + radius * Math.sin(radians) };
  };
  return { pure: at(hue), white: at(hue + 120), black: at(hue + 240) };
}

export function barycentric(p: Point, { pure, white, black }: TriangleVertices): [number, number, number] {
  const denominator = (white.y - black.y) * (pure.x - black.x) + (black.x - white.x) * (pure.y - black.y);
  const a = ((white.y - black.y) * (p.x - black.x) + (black.x - white.x) * (p.y - black.y)) / denominator;
  const b = ((black.y - pure.y) * (p.x - black.x) + (pure.x - black.x) * (p.y - black.y)) / denominator;
  return [a, b, 1 - a - b];
}

/** Converte um ponto (dentro ou perto do triângulo) em saturação e brilho; fora do triângulo, gruda na borda. */
export function svFromPoint(p: Point, vertices: TriangleVertices): { s: number; v: number } {
  let [a, b, c] = barycentric(p, vertices);
  a = Math.max(0, a); b = Math.max(0, b); c = Math.max(0, c);
  const total = a + b + c || 1;
  a /= total; b /= total; c /= total;
  const v = a + b;
  return { s: v > 1e-6 ? a / v : 0, v: clamp(v, 0, 1) };
}

export function pointFromSv(s: number, v: number, { pure, white, black }: TriangleVertices): Point {
  const a = s * v, b = v * (1 - s), c = 1 - v;
  return { x: a * pure.x + b * white.x + c * black.x, y: a * pure.y + b * white.y + c * black.y };
}

/** Matiz (0–360) de um ponto relativo ao centro. */
export function hueFromPoint(p: Point, center: Point): number {
  const degrees = (Math.atan2(p.y - center.y, p.x - center.x) * 180) / Math.PI;
  return (degrees + 360) % 360;
}
