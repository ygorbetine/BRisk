import { useCallback, useEffect, useRef } from "react";
import { barycentric, hexToHsv, hsvToHex, hsvToRgb, hueFromPoint, pointFromSv, svFromPoint, triangleVertices, type Hsv, type Point } from "../services/theme/color";

const SIZE = 200;
const CENTER: Point = { x: SIZE / 2, y: SIZE / 2 };
const OUTER = SIZE / 2 - 2;
const RING = 16;
const INNER = OUTER - RING - 4;

type Props = { value: string; onChange: (hex: string) => void; label: string };

/** Seletor de cor: anel de matiz + triângulo de saturação/brilho (vértices: cor pura, branco, preto). */
export function TrianglePicker({ value, onChange, label }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const hsvRef = useRef<Hsv>(hexToHsv(value));
  const dragRef = useRef<"ring" | "triangle" | null>(null);

  // Mantém matiz/saturação quando o hex vem de fora; só ressincroniza se a cor mudou de verdade.
  if (hsvToHex(hsvRef.current) !== value) hsvRef.current = hexToHsv(value);

  const draw = useCallback(() => {
    const canvas = canvasRef.current;
    const context = canvas?.getContext("2d");
    if (!canvas || !context) return;
    const ratio = window.devicePixelRatio || 1;
    canvas.width = SIZE * ratio; canvas.height = SIZE * ratio;
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    context.clearRect(0, 0, SIZE, SIZE);
    const hsv = hsvRef.current;

    // Anel de matiz
    const ring = context.createConicGradient(0, CENTER.x, CENTER.y);
    for (let step = 0; step <= 12; step++) {
      const rgb = hsvToRgb({ h: (step / 12) * 360, s: 1, v: 1 });
      ring.addColorStop(step / 12, `rgb(${Math.round(rgb.r)} ${Math.round(rgb.g)} ${Math.round(rgb.b)})`);
    }
    context.beginPath();
    context.arc(CENTER.x, CENTER.y, OUTER, 0, Math.PI * 2);
    context.arc(CENTER.x, CENTER.y, OUTER - RING, 0, Math.PI * 2, true);
    context.fillStyle = ring;
    context.fill("evenodd");

    // Triângulo S/V: base na cor pura, mistura vertical com preto e horizontal com branco
    const vertices = triangleVertices(CENTER, INNER, hsv.h);
    const pure = hsvToRgb({ h: hsv.h, s: 1, v: 1 });
    context.save();
    context.beginPath();
    context.moveTo(vertices.pure.x, vertices.pure.y);
    context.lineTo(vertices.white.x, vertices.white.y);
    context.lineTo(vertices.black.x, vertices.black.y);
    context.closePath();
    context.clip();
    // Mistura exata por pixel (coordenadas baricêntricas): a·pura + b·branco + c·preto
    const pixels = Math.ceil(SIZE * ratio);
    const image = context.createImageData(pixels, pixels);
    const scale = SIZE / pixels;
    for (let py = 0; py < pixels; py++) {
      for (let px = 0; px < pixels; px++) {
        const [a, b, c] = barycentric({ x: (px + 0.5) * scale, y: (py + 0.5) * scale }, vertices);
        if (a < -0.02 || b < -0.02 || c < -0.02) continue;
        const offset = (py * pixels + px) * 4;
        image.data[offset] = Math.min(255, Math.max(0, a * pure.r + b * 255));
        image.data[offset + 1] = Math.min(255, Math.max(0, a * pure.g + b * 255));
        image.data[offset + 2] = Math.min(255, Math.max(0, a * pure.b + b * 255));
        image.data[offset + 3] = 255;
      }
    }
    const layer = document.createElement("canvas");
    layer.width = pixels; layer.height = pixels;
    layer.getContext("2d")?.putImageData(image, 0, 0);
    context.drawImage(layer, 0, 0, SIZE, SIZE);
    context.restore();

    // Marcadores
    const hueAngle = (hsv.h * Math.PI) / 180;
    const ringRadius = OUTER - RING / 2;
    const marker = (point: Point, radius: number, fill: string) => {
      context.beginPath();
      context.arc(point.x, point.y, radius, 0, Math.PI * 2);
      context.fillStyle = fill;
      context.fill();
      context.lineWidth = 2;
      context.strokeStyle = "#fff";
      context.stroke();
      context.beginPath();
      context.arc(point.x, point.y, radius + 1.5, 0, Math.PI * 2);
      context.lineWidth = 1;
      context.strokeStyle = "rgba(0,0,0,.45)";
      context.stroke();
    };
    marker({ x: CENTER.x + ringRadius * Math.cos(hueAngle), y: CENTER.y + ringRadius * Math.sin(hueAngle) }, 6, `rgb(${Math.round(pure.r)} ${Math.round(pure.g)} ${Math.round(pure.b)})`);
    marker(pointFromSv(hsv.s, hsv.v, vertices), 6, value);
  }, [value]);

  useEffect(draw, [draw]);

  const update = (event: React.PointerEvent<HTMLCanvasElement>, mode: "ring" | "triangle") => {
    const rect = event.currentTarget.getBoundingClientRect();
    const point: Point = { x: ((event.clientX - rect.left) / rect.width) * SIZE, y: ((event.clientY - rect.top) / rect.height) * SIZE };
    const current = hsvRef.current;
    const next: Hsv = mode === "ring"
      ? { ...current, h: hueFromPoint(point, CENTER) }
      : { ...current, ...svFromPoint(point, triangleVertices(CENTER, INNER, current.h)) };
    hsvRef.current = next;
    onChange(hsvToHex(next));
  };

  const onPointerDown = (event: React.PointerEvent<HTMLCanvasElement>) => {
    const rect = event.currentTarget.getBoundingClientRect();
    const x = ((event.clientX - rect.left) / rect.width) * SIZE - CENTER.x;
    const y = ((event.clientY - rect.top) / rect.height) * SIZE - CENTER.y;
    const distance = Math.hypot(x, y);
    if (distance < INNER - 8 || distance < OUTER - RING - 2) dragRef.current = "triangle";
    else if (distance <= OUTER + 4) dragRef.current = "ring";
    else return;
    event.currentTarget.setPointerCapture(event.pointerId);
    update(event, dragRef.current);
  };

  const onKeyDown = (event: React.KeyboardEvent<HTMLCanvasElement>) => {
    const step = event.shiftKey ? 10 : 2;
    const current = hsvRef.current;
    let next: Hsv | null = null;
    if (event.key === "ArrowLeft") next = { ...current, h: (current.h - step + 360) % 360 };
    else if (event.key === "ArrowRight") next = { ...current, h: (current.h + step) % 360 };
    else if (event.key === "ArrowUp") next = { ...current, v: Math.min(1, current.v + step / 100) };
    else if (event.key === "ArrowDown") next = { ...current, v: Math.max(0, current.v - step / 100) };
    if (!next) return;
    event.preventDefault();
    hsvRef.current = next;
    onChange(hsvToHex(next));
  };

  return <canvas
    ref={canvasRef}
    className="triangle-picker"
    style={{ width: SIZE, height: SIZE, touchAction: "none" }}
    tabIndex={0}
    role="slider"
    aria-label={label}
    aria-valuetext={value}
    aria-valuenow={Math.round(hsvRef.current.h)}
    aria-valuemin={0}
    aria-valuemax={360}
    onPointerDown={onPointerDown}
    onPointerMove={(event) => { if (dragRef.current) update(event, dragRef.current); }}
    onPointerUp={() => { dragRef.current = null; }}
    onPointerCancel={() => { dragRef.current = null; }}
    onKeyDown={onKeyDown}
  />;
}
