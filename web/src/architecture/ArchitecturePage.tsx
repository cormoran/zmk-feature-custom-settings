import { Diagram, Flow, OwnershipMap } from "./Diagrams";
import { Source } from "./Source";
import { implementationRevision, readingMap, reviewNotes } from "./content";
import { ValueLayoutSection } from "./ValueLayoutSection";
import { LifetimeSection } from "./LifetimeSection";
import { StorageSection } from "./StorageSection";
import { ResultsSection } from "./ResultsSection";

const chapters = [
  ["overview", "まず全体像"],
  ["code", "コードの読み順"],
  ["layout", "型とメモリ配置"],
  ["lifetime", "参照とコピーの寿命"],
  ["storage", "配列・pool・keyspace"],
  ["rpc", "RPC の境界"],
  ["results", "実測値の読み方"],
  ["review", "レビューで注意する点"],
];

export default function ArchitecturePage() {
  return (
    <div className="architecture-page">
      <a className="skip-link" href="#guide-main">
        本文へ移動
      </a>
      <header className="guide-header">
        <a href="./">← Settings console</a>
        <span>DEVELOPER GUIDE / 日本語</span>
      </header>
      <div className="guide-shell">
        <nav className="guide-nav" aria-label="解説の目次">
          <strong>コードとメモリを読む</strong>
          {chapters.map(([id, title], index) => (
            <a key={id} href={`#${id}`}>
              <span>0{index + 1}</span>
              {title}
            </a>
          ))}
          <small>
            デバイス接続不要
            <br />
            図は概念図。サイズは ARM32。
          </small>
        </nav>
        <main id="guide-main">
          <section id="overview" className="guide-hero">
            <p className="eyebrow">ZMK CUSTOM SETTINGS · MEMORY REDESIGN</p>
            <h1>
              値を運ぶ型と、
              <br />
              値を保存する場所を分ける。
            </h1>
            <p className="lead">
              8 B の value は設定値を受け渡すための器です。設定の説明は
              descriptor、現在値は state、可変長データは ROM または pool
              にあります。この区別からコードを読むと、小さくできた理由と、ポインタを使う時の制約がつながります。
            </p>
            <div className="guide-facts">
              <div>
                <strong>8 B</strong>
                <span>共通 value view</span>
              </div>
              <div>
                <strong>512 B</strong>
                <span>共有 pool の既定予算</span>
              </div>
              <div>
                <strong>129,352 B</strong>
                <span>DYA2 右通常・互換 ON</span>
              </div>
            </div>
            <p className="provenance">
              対象実装 <Source path="">{implementationRevision}</Source>
              。計測はビルド時の静的 RAM
              使用量。実機のピーク使用量ではありません。
            </p>
            <Diagram
              title="01 — 説明・保存・受け渡しは別の役割"
              caption="上の矢印は descriptor の state 参照、下は読み出し時の view 生成。state が view を保存するわけではありません。動的 keyspace の descriptor と配列要素の一時 descriptor は RAM にあります。"
            >
              <OwnershipMap />
            </Diagram>
          </section>

          <section id="code">
            <h2>02 / どのファイルから読むか</h2>
            <p>
              公開型から入り、値の選択、保存領域、通信の順に進みます。内部
              helper の共通契約は{" "}
              <Source path="src/custom_settings_internal.h" />{" "}
              にあります。原則として呼び出し側が settings lock を保持します。
            </p>
            <Diagram
              title="02 — 呼び出しの層と互換境界"
              caption="旧 API は compat を経由して共通エンジンへ。allocator は descriptor、protobuf、設定名を扱いません。"
            >
              <Flow
                items={[
                  {
                    title: "呼び出し側",
                    detail: "新 API / ref API、または旧 API → compat",
                  },
                  {
                    title: "共通エンジン",
                    detail: "検証・現在値の選択・変更モード・lock",
                  },
                  {
                    title: "型別の保存",
                    detail: "scalar state / array storage / keyspace blob",
                  },
                  {
                    title: "byte の管理",
                    detail: "blob adapter → pool allocator",
                  },
                ]}
              />
            </Diagram>
            <div className="file-grid">
              {readingMap.map((group) => (
                <article className="file-card" key={group.title}>
                  <h3>{group.title}</h3>
                  <p>{group.description}</p>
                  <ul>
                    {group.files.map((path) => (
                      <li key={path}>
                        <Source path={path} />
                      </li>
                    ))}
                  </ul>
                </article>
              ))}
            </div>
            <p className="note">
              新しい値型を追加するなら、公開 enum と view だけでなく、state
              選択・array storage・raw codec・validation・RPC・永続化の各 switch
              を確認します。8 B
              になったこと自体は、新しい型が自動的に全経路で扱えることを意味しません。
            </p>
          </section>

          <ValueLayoutSection />

          <LifetimeSection />

          <StorageSection />

          <section id="rpc">
            <h2>06 / 通信との境界</h2>
            <Diagram
              title="10 — encode のためだけに値全体を複製しない"
              caption="converter がない値は lock 中の visitor から encode。converter がある経路は呼び出し側 buffer へコピーしてから、従来どおり converter を lock 外で呼びます。"
            >
              <Flow
                items={[
                  {
                    title: "request decode",
                    detail: "BEHAVIOR 実体は request handler が所有",
                  },
                  {
                    title: "設定 API",
                    detail: "ref で解決、検証、read / write",
                  },
                  {
                    title: "response encode",
                    detail: "borrowed view または converter 用コピー",
                  },
                  {
                    title: "応答完了",
                    detail: "共有 response を次の request が利用",
                  },
                ]}
              />
            </Diagram>
            <p>
              共有応答は request / encode
              の直列処理が前提です。将来並行処理に変える時は response
              の所有権も再設計が必要です。単一フレームの上限は既定 64 B。STRING
              用の余分な NUL 1 B が buffer にあっても、通信の上限は増えません。
            </p>
            <p>
              chunk の入力 buffer は非同期の複数 request
              にまたがるため、同期編集 workspace と共用しません。chunk
              version・timeout・disconnect 処理、Flash
              worker、複数レコードの原子的保存は今回の実装範囲外です。
            </p>
          </section>

          <ResultsSection />

          <section id="review">
            <h2>08 / 可読性レビューで残した判断</h2>
            <p>
              責務別ファイルと compat
              の分離は有効です。一方、省メモリのために型だけでは所有権を表せない箇所が残ります。レビューでは以下の契約を重点的に確認します。
            </p>
            <div className="review-list">
              {reviewNotes.map((note) => (
                <article key={note.title}>
                  <span className="review-status">{note.status}</span>
                  <h3>{note.title}</h3>
                  <p>{note.text}</p>
                </article>
              ))}
            </div>
            <h3>変更内容に応じた確認先</h3>
            <ul>
              <li>
                値の型・寿命：
                <Source path="src/test/custom_settings_test.c" /> の compact
                value / behavior ケース。
              </li>
              <li>
                配置と互換切り替え：
                <Source path="src/test/compact_config_sample_settings.c" /> の
                ARM BUILD_ASSERT。
              </li>
              <li>
                pool の所有権：
                <Source path="src/custom_settings_allocator.c" /> と{" "}
                <Source path="test.py" /> の host allocator 検証。
              </li>
              <li>
                レビュー記録：今回の判断と未解決事項（
                <a
                  href="https://github.com/cormoran/zmk-feature-custom-settings/blob/impl/memory-redesign/docs/design/code-readability-review.md"
                  target="_blank"
                  rel="noreferrer"
                >
                  作業ブランチで開く
                </a>
                ）。
              </li>
            </ul>
          </section>
          <footer>
            サイズとリンクの基準 revision は{" "}
            <code>src/architecture/content.ts</code>{" "}
            に集約しています。実装を更新したら、契約・図・計測条件を一緒に見直してください。
          </footer>
        </main>
      </div>
    </div>
  );
}
