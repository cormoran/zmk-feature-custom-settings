import { Fragment } from "react";
import type { ReactNode } from "react";

export function Diagram({
  title,
  children,
  caption,
}: {
  title: string;
  children: ReactNode;
  caption: string;
}) {
  return (
    <figure className="diagram" aria-label={title}>
      <h3>{title}</h3>
      {children}
      <figcaption>{caption}</figcaption>
    </figure>
  );
}

export function Flow({
  items,
}: {
  items: { title: string; detail: string }[];
}) {
  return (
    <ol className="flow">
      {items.map((item, index) => (
        <li key={item.title}>
          <span className="step-number">{index + 1}</span>
          <strong>{item.title}</strong>
          <span>{item.detail}</span>
        </li>
      ))}
    </ol>
  );
}

export function MemoryStrip({
  parts,
}: {
  parts: { label: string; bytes: number; kind?: string }[];
}) {
  return (
    <div className="memory-strip">
      {parts.map((part) => (
        <div
          key={part.label}
          className={`memory-part ${part.kind ?? "ram"}`}
          style={{ flexGrow: part.bytes }}
        >
          <strong>{part.label}</strong>
          <span>{part.bytes} B</span>
        </div>
      ))}
    </div>
  );
}

export function OwnershipMap() {
  return (
    <div className="ownership-map">
      {[
        [
          "登録 descriptor",
          "名前・型・default・state の参照",
          "ROM ※静的登録",
          "rom",
        ],
        [
          "型別 state",
          "flags と現在の値／blob node",
          "RAM・寿命は設定と同じ",
          "ram",
        ],
        [
          "value view",
          "type・size・数値 または pointer",
          "一時的に値を運ぶ 8 B",
          "view",
        ],
      ].map(([name, detail, place, color], i) => (
        <Fragment key={name}>
          {i > 0 && (
            <span className="map-arrow" aria-hidden="true">
              {i === 1 ? "↓ state pointer" : "↓ 現在値を読み取る"}
            </span>
          )}
          <div className={`memory-part ${color}`}>
            <strong>{name}</strong>
            <span>{detail}</span>
            <small>{place}</small>
          </div>
        </Fragment>
      ))}
      <div className="payload-branches">
        <div className="memory-part rom">
          <strong>ROM の既定値</strong>
          <span>未編集 blob が参照</span>
        </div>
        <div className="memory-part ram">
          <strong>RAM の実データ</strong>
          <span>scalar state / 配列 / pool / 一時 slot</span>
        </div>
      </div>
    </div>
  );
}
