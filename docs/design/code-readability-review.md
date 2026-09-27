# コード構造・メモリ構造の可読性レビュー

対象は memory redesign の実装 `e952906`。公開 header、共通エンジン、型別保存、
allocator、ref/keyspace、互換境界、RPC の値の受け渡しを読み直した記録です。
実装の理解に必要な図は `web/architecture.html` にまとめています。

## 今回改善した点

- `with_view` の宣言直前に、借用 pointer の寿命と settings API の再入禁止を記載。
  従来は離れた `with_default_view` のコメントまで読まないと契約が見つかりにくかった。
  lock が recursive でも、共通 scratch と移動可能な pool は再入に対して安全ではない。
- private header の `copy_value` に size の capacity → length の変化を、
  `effective_value` に共有 scratch の上書き条件を記載。
- keyspace の冒頭コメントに残っていた削除済み `custom_settings_pool.c` への参照と、
  「このファイルだけが keyspace を知る」という現状に合わない説明を修正。
- Web に独立した説明ページを追加。コード地図、所有権、byte 配置、処理順と寿命を
  別の図にして、一枚の図に意味の違う矢印を詰め込まない構成にした。
  firmware の動作・公開 API・メモリ配置はこのレビューでは変更していない。

## 維持する分離

- `value_types.h` は値の表現、`state.h` は保存先、`descriptor.h` は登録情報。
  同じ「設定」に属しても所有者と寿命が違うため、統合しない。
- `array.c` は操作、`array_storage.c` は型別の格納と bitset。
  要素の移動と保存先 ordinal のフラグを区別できる現在の分離を維持する。
- `blob.c` は設定から allocator への adapter。allocator は pool と node のみを扱う。
  同期 workspace の利用と compact の前提はこの境界で確認できる。
- `compat/value_api.c` は旧関数のラッパー、`compat/value.c` は所有データの変換。
  繰り返しはあるが、C の複雑な macro で統合すると debug と契約確認が難しくなるため、
  このレビューでは明示的な関数を維持する。

## 残る読みづらさと改善候補

| 論点 | 現状の注意 | 次に変更する場合 |
| --- | --- | --- |
| view が入力・借用・出力を兼ねる | `size` は出力時だけ capacity → length。再利用前に初期化が必要 | コピー先を pointer + capacity の別引数にする API を検討。API 移行とサイズ計測を一緒に行う |
| `with_view` と `ref_visit` | 前者は値を借りるので再入禁止、後者は同期 settings API を呼べる | callback の名前と公開 API の文書を揃え、契約を型だけから推測させない |
| state accessor の例外 | keyspace は公開型に関係なく blob。配列は要素領域を使う | 新しい経路では raw state に直接触れず、既存の read/write 境界を使う |
| 旧名の macro alias | OFF では名前だけ旧 API、契約は compact API | 新規 consumer は `_view` / `_ref` を明示。移行時に旧出力の初期化を監査する |
| `custom_settings.c` の責務 | 値の選択・検証・永続化・scope 操作が一つに残る | 永続化の分離を候補にする。lock、shared scratch、単一保存失敗時の不変条件を先に固定する |
| private helper の命名 | `copy_value` / `effective_value` だけでは所有権が分からない | 次の責務分割時に命名を統一。独立した一括 rename で差分だけ増やさない |

## レビュー時の確認順

1. 値を運ぶ view と、その pointer が指す実データの所有者を確認する。
2. lock の取得から visitor 終了までに再入・待機・pointer 保存がないかを見る。
3. state の種類を scalar / array / keyspace で分ける。
4. pool reserve の前に入力を退避しているか、失敗前に旧値を変更していないかを見る。
5. 互換 OFF で使わない翻訳単位が CMake から除外されているかを見る。
6. sizeof の差と ELF 全体の RAM 差を混同しない。ROM metadata、別 payload、padding、
   予約済み stack、共有 pool の容量条件まで含めて結果を解釈する。

このレビューは上記経路の可読性・契約の監査であり、全経路の形式的な正しさの証明ではありません。
