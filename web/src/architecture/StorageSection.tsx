import { useState } from "react";
import { Diagram, Flow } from "./Diagrams";

export function StorageSection() {
  const [edited, setEdited] = useState(false);
  return (
    <section id="storage">
      <h2>05 / 配列と可変長データの置き場所</h2>
      <Diagram
        title="07 — 配列は要素ごとの大きな descriptor を持たない"
        caption="INT32 の例。値は 4 B × capacity の連続領域。状態フラグは bitset。旧 API の view cache は互換 ON の時だけ別途存在します。"
      >
        <Flow
          items={[
            {
              title: "配列親",
              detail: "共通 state + array_state（長さ・容量・参照）",
            },
            {
              title: "型別要素領域",
              detail: "[ int32 ][ int32 ][ int32 ] …",
            },
            {
              title: "要素を操作",
              detail: "親と index から一時 descriptor を作る",
            },
          ]}
        />
        <p className="callout">
          ref の index は「その位置」を指します。insert / remove
          後も同じ値を追跡する ID
          ではありません。永続化済みフラグも値ではなく保存先の ordinal
          に対応します。
        </p>
      </Diagram>
      <Diagram
        title="08 — blob の初期値と編集後"
        caption="模式例：4 byte の STRING。node は固定位置、payload は移動可能。pool の管理 node 自体や workspace は、512 B の payload 予算とは別の RAM です。"
      >
        <div className="mode-control">
          <button
            type="button"
            aria-pressed={edited}
            onClick={() => setEdited(!edited)}
          >
            {edited ? "既定値の状態へ戻す" : "編集後の配置を見る"}
          </button>
        </div>
        <div aria-live="polite">
          <Flow
            items={[
              {
                title: "blob node（12 B）",
                detail: edited
                  ? "data → pool、size=4、extent=5"
                  : "data → ROM、size=4、extent=0",
              },
              {
                title: edited ? "RAM pool" : "ROM default",
                detail: edited
                  ? '"edit\\0"：5 B を確保'
                  : '"init"：pool 使用量 0 B',
              },
            ]}
          />
        </div>
        <p>
          書き換え順：入力を workspace に退避 → reserve / compact → node の data
          を更新 → payload をコピー。拡張時は別の blob の bytes も動くため、node
          から取得した pointer を保存しないこと。
        </p>
      </Diagram>
      <p>
        pool は既定 512 B
        を共有します。全設定の最大長を足した容量ではありません。reserve 失敗は{" "}
        <code>-ENOSPC</code> で元の node と byte を維持します。設定上限・pool
        予算・一時変更用 slot 容量は別の制限です。
      </p>
      <Diagram
        title="09 — keyspace は公開型と保存型が違う"
        caption="slot の state は常に blob。名前と payload を一緒に保持し、読み出し境界で公開型に decode します。BEHAVIOR の decode 用実体は caller local に置き、visitor が終わるまで生存させます。"
      >
        <Flow
          items={[
            {
              title: "keyspace slot",
              detail: "descriptor + blob state + generation",
            },
            {
              title: "保存 bytes",
              detail: "[ user key ][ NUL ][ encoded payload ]",
            },
            {
              title: "公開する view",
              detail: "INT32 / BOOL / BEHAVIOR / STRING / BYTES",
            },
          ]}
        />
        <p className="callout">
          slot を削除・再利用すると generation が変わり、古い ref は{" "}
          <code>-ESTALE</code>。raw pointer の再利用判定に頼りません。
        </p>
      </Diagram>
    </section>
  );
}
