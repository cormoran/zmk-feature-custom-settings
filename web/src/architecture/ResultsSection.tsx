import { Diagram, Flow } from "./Diagrams";
import { Source } from "./Source";
import { measurements } from "./content";

export function ResultsSection() {
  return (
    <section id="results">
      <h2>07 / RAM が減った量と、その意味</h2>
      <p>
        DYA2 の依存と構成を固定し、ELF の <code>_image_ram_size</code>{" "}
        を比較しました。予約済み thread stack・heap・alignment を含み、stack
        high-water の計測ではありません。
      </p>
      <div className="table-scroll">
        <table>
          <caption>DYA2：全構成で互換 ON（単位 B）</caption>
          <thead>
            <tr>
              <th>構成</th>
              <th>再設計前 c6a7fef</th>
              <th>直前 03ab1b0</th>
              <th>今回 e952906</th>
              <th>再設計前との差</th>
            </tr>
          </thead>
          <tbody>
            {measurements.map((row) => (
              <tr key={row.name}>
                <th>{row.name}</th>
                <td>{row.before.toLocaleString("en-US")}</td>
                <td>{row.previous.toLocaleString("en-US")}</td>
                <td>
                  <b>{row.after.toLocaleString("en-US")}</b>
                </td>
                <td>−{(row.before - row.after).toLocaleString("en-US")}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <Diagram
        title="11 — 今回の value 縮小による差分"
        caption="DYA2 の静的 scratch 二つが対象。136 B の変数削減に padding の増加 8 B が加わり、ELF 全体では各構成 128 B 減。設定数 × 68 B という計算にはなりません。"
      >
        <Flow
          items={[
            { title: "変更前", detail: "76 B × 2 = 152 B" },
            { title: "変更後", detail: "8 B × 2 = 16 B" },
            {
              title: "全体の差",
              detail: "−136 B + padding 8 B = −128 B",
            },
          ]}
        />
      </Diagram>
      <p>
        DYA2 は旧形式を使う consumer が残るため互換 ON です。OFF の state
        縮小はこの結果に含みません。同一設定の単体 ARM fixture では、ON 70,702 B
        → OFF 69,806 B（−896 B）。構成全体の比較で、state
        単独の効果ではありません。
      </p>
      <details>
        <summary>再現条件・検証範囲を開く</summary>
        <ul>
          <li>
            DYA2 eca79a3 / ZMK e5c9b6915b56801193e359dd9bad4a167ce0d1b8 / Zephyr
            7c6b4cc486ecb41a68d9c2b1def2bb3178fbb826
          </li>
          <li>
            VALUE_MAX_SIZE=64、LARGE_VALUE_MAX_SIZE=256、combo 上限 8。共有 pool
            は新実装で 512 B。
          </li>
          <li>
            ARM 9 構成・native core / Studio / split peripheral の互換 ON/OFF 計
            6 suite・allocator UBSan 20,000 操作・DYA2 4 UF2。
          </li>
          <li>実機操作、stack high-water、電源断・切断試験は未実施。</li>
        </ul>
        <Source path="docs/design/memory-redesign-implementation.html">
          元の詳細レビュー HTML（ソース）
        </Source>
      </details>
    </section>
  );
}
