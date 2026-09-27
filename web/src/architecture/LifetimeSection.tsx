import { Diagram, Flow } from "./Diagrams";
import { Source } from "./Source";
import { publicHeaders } from "./content";

export function LifetimeSection() {
  return (
    <section id="lifetime">
      <h2>04 / ポインタはいつまで使えるか</h2>
      <p>
        同じ view 型でも、borrowed read と copying read
        では寿命が異なります。関数名と buffer の所有者をセットで読みます。
      </p>
      <Diagram
        title="05 — 借用する読み出し：with_view"
        caption="visitor 内だけ有効。pool の移動や共通 scratch の上書きを防ぐため、visitor から settings API を呼ばず、pointer を外へ持ち出しません。"
      >
        <Flow
          items={[
            { title: "lock を取る", detail: "現在値を解決" },
            {
              title: "view を借りる",
              detail: "state / pool / temporary / decode 用 local",
            },
            {
              title: "visitor で使う",
              detail: "その場で encode、または必要な分をコピー",
            },
            { title: "unlock", detail: "借用参照の利用終了" },
          ]}
        />
      </Diagram>
      <Diagram
        title="06 — コピーする読み出し：read_view"
        caption="出力 buffer は呼び出し側が所有。コピー後はその buffer の寿命まで利用できます。STRING の size は終端 NUL を含みません。"
      >
        <Flow
          items={[
            {
              title: "buffer を用意",
              detail:
                "size = 容量。STRING は NUL 分、BEHAVIOR は alignment も必要",
            },
            {
              title: "lock 中にコピー",
              detail: "容量不足は -EMSGSIZE。array remove も削除前に失敗",
            },
            {
              title: "unlock 後に使う",
              detail: "size = payload 長。再読込時は buffer と容量を再設定",
            },
          ]}
        />
      </Diagram>
      <pre>
        <code>{`char text[65];
struct zmk_custom_setting_value_view value =
    ZMK_CUSTOM_SETTING_VIEW_BUFFER(text, sizeof(text));
int err = zmk_custom_setting_read_view(setting, &value);
if (err == 0) {
    /* STRING 設定なら text に NUL 終端付きコピー。 */
}
/* 次の read の前に VIEW_BUFFER で再初期化する。 */`}</code>
      </pre>
      <p>
        入力の STRING / BYTES / BEHAVIOR も pointer を借ります。write
        呼び出しが終わるまで参照先を生存させます。constructor の compound
        literal
        はブロック内ならそのブロックの終わりまでで、関数から返して持ち回る用途には使えません。
      </p>
      <p className="note">
        <b>ref_visit は別の契約です。</b> ref を一時 descriptor に解決する
        visitor なので、同期 settings API を呼べます。解決した descriptor
        の保存や worker 待ちは禁止です。値を借りる with_view
        と混同しないでください。
        <Source path={`${publicHeaders}ref.h`}>ref.h の契約</Source>
      </p>
    </section>
  );
}
