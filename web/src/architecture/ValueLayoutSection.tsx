import { useState } from "react";
import { Diagram, Flow, MemoryStrip } from "./Diagrams";
import { Source } from "./Source";
import { publicHeaders, stateRows } from "./content";

export function ValueLayoutSection() {
  const [legacy, setLegacy] = useState(false);
  return (
    <section id="layout">
      <h2>03 / 何が何 byte を使うか</h2>
      <p>
        互換設定を切り替えて、残る型を比較できます。これは説明図の切り替えで、ファームウェア設定の変更ではありません。
      </p>
      <fieldset className="mode-control">
        <legend>CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT</legend>
        <label>
          <input
            type="radio"
            name="legacy"
            checked={!legacy}
            onChange={() => setLegacy(false)}
          />{" "}
          OFF — 共通型のみ
        </label>
        <label>
          <input
            type="radio"
            name="legacy"
            checked={legacy}
            onChange={() => setLegacy(true)}
          />{" "}
          ON — 旧 API を維持（既定）
        </label>
      </fieldset>
      <div aria-live="polite">
        <Diagram
          title="03 — value の実際の配置"
          caption="ARM32 の alignment を含みます。union は合計ではなく最大メンバー分。BEHAVIOR の 12 B payload は参照先に存在し、消えるわけではありません。"
        >
          <p>
            <code>zmk_custom_setting_value_view</code>：常に <b>8 B</b>
          </p>
          <MemoryStrip
            parts={[
              { label: "type", bytes: 1 },
              { label: "padding", bytes: 1, kind: "padding" },
              { label: "size", bytes: 2 },
              { label: "数値 / pointer", bytes: 4, kind: "view" },
            ]}
          />
          {legacy ? (
            <>
              <p>
                <code>zmk_custom_setting_value</code>：旧データ内包型{" "}
                <b>76 B</b>（VALUE_MAX_SIZE=64）
              </p>
              <MemoryStrip
                parts={[
                  { label: "enum type", bytes: 4 },
                  { label: "size_t size", bytes: 4 },
                  {
                    label: "union（最大 STRING 65）",
                    bytes: 65,
                    kind: "legacy",
                  },
                  { label: "padding", bytes: 3, kind: "padding" },
                ]}
              />
            </>
          ) : (
            <p className="callout">
              <code>zmk_custom_setting_value</code> は <code>value_view</code>{" "}
              の別名。二つの struct 定義も、API wrapper もありません。
            </p>
          )}
        </Diagram>
        <Diagram
          title="04 — descriptor が state の先頭を指す"
          caption="OFF は flags の直後に型別 payload を置くため、state 内の追加 pointer は不要です。BOOL は 2 B、INT32 は alignment を含め 8 B。"
        >
          <Flow
            items={[
              {
                title: `descriptor ${legacy ? "52" : "40"} B`,
                detail: legacy
                  ? "旧 metadata / state の配置を保持"
                  : "名前・型・metadata 参照・state pointer",
              },
              {
                title: legacy ? "旧 state 20 B" : "型別 state",
                detail: legacy
                  ? "最大 payload を持つ union と旧機能用 field"
                  : "flags + 必要な型の payload だけ",
              },
            ]}
          />
          <div className="table-scroll">
            <table>
              <caption>
                {legacy ? "互換 ON の state" : "互換 OFF の state"}
                （ARM32）
              </caption>
              <thead>
                <tr>
                  <th>種類</th>
                  <th>確保サイズ</th>
                  <th>OFF の内訳</th>
                </tr>
              </thead>
              <tbody>
                {stateRows.map((row) => (
                  <tr key={row.name}>
                    <th>{row.name}</th>
                    <td>{legacy ? row.before : row.after} B</td>
                    <td>{row.layout}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </Diagram>
      </div>
      <p>
        descriptor は通常 ROM の静的登録情報です。52 → 40 B
        の差を設定数に掛けても、そのまま RAM 削減量にはなりません。OFF では
        metadata が別オブジェクトになる点も含め、ELF 全体で比較します。
      </p>
      <p>
        定義：
        <Source path={`${publicHeaders}value_types.h`} /> /{" "}
        <Source path={`${publicHeaders}state.h`} />
        。旧 layout は{" "}
        <Source path={`${publicHeaders}compat/value.h`}>
          compat/value.h
        </Source>{" "}
        と{" "}
        <Source path={`${publicHeaders}compat/state.h`}>compat/state.h</Source>
        。
      </p>
    </section>
  );
}
