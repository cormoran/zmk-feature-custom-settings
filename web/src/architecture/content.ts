// Source links identify the implementation that the measurements describe.
// Update this revision and the measurements together after firmware changes.
export const implementationRevision = "e952906";
export const sourceRoot = `https://github.com/cormoran/zmk-feature-custom-settings/blob/${implementationRevision}/`;
export const publicHeaders = "include/cormoran/zmk/custom_settings/";

export const readingMap = [
  {
    title: "1. 型と公開 API",
    files: [
      "include/cormoran/zmk/custom_settings.h",
      `${publicHeaders}value_types.h`,
      `${publicHeaders}value_view.h`,
      `${publicHeaders}descriptor.h`,
      `${publicHeaders}state.h`,
    ],
    description:
      "登録情報、値を運ぶ型、保存領域を区別する入口。個別 header は umbrella header から読む想定です。",
  },
  {
    title: "2. 値の選択とコピー",
    files: ["src/custom_settings.c", "src/custom_settings_value.c"],
    description:
      "一時値の優先、検証、保存・破棄を core が調停。value.c は呼び出し側 buffer へのコピーと長さ検査を担当します。",
  },
  {
    title: "3. 配列",
    files: [
      "src/custom_settings_array.c",
      "src/custom_settings_array_storage.c",
      "src/custom_settings_views.h",
    ],
    description:
      "配列操作と、型別データ・bitset の操作を分離。要素 descriptor はその場で組み立てます。",
  },
  {
    title: "4. 可変長データ",
    files: [
      "src/custom_settings_blob.c",
      "src/custom_settings_allocator.c",
      `${publicHeaders}pool.h`,
    ],
    description:
      "blob.c は編集 workspace と保存領域を接続。allocator.c は設定名や RPC を知らず、連続 byte 領域を管理します。",
  },
  {
    title: "5. 識別と動的キー",
    files: [
      "src/custom_settings_ref.c",
      "src/custom_settings_keyspace.c",
      `${publicHeaders}ref.h`,
    ],
    description:
      "ref は持ち回れる識別子。keyspace は名前と payload を一つの blob に格納し、公開する型に変換します。",
  },
  {
    title: "6. 通信と旧 API",
    files: [
      "src/studio/custom_settings_handler.c",
      "src/custom_settings_rpc_convert.c",
      "src/compat/value_api.c",
      "src/compat/value.c",
      "CMakeLists.txt",
    ],
    description:
      "handler は protobuf 境界、converter は byte 変換。旧 API のラッパーと所有データの変換は compat に閉じ込めます。",
  },
];

export const stateRows = [
  { name: "BOOL", before: 20, after: 2, layout: "flags 1 + bool 1" },
  {
    name: "INT32",
    before: 20,
    after: 8,
    layout: "flags 1 + padding 3 + int32 4",
  },
  {
    name: "BEHAVIOR",
    before: 20,
    after: 16,
    layout: "flags 1 + padding 3 + behavior 12",
  },
  {
    name: "BYTES / STRING",
    before: 20,
    after: 16,
    layout: "flags 1 + padding 3 + blob node 12",
  },
  {
    name: "配列親の共通 state",
    before: 20,
    after: 1,
    layout: "flags のみ。array_state と要素領域は別途必要",
  },
];

export const measurements = [
  { name: "右・通常", before: 161576, previous: 129480, after: 129352 },
  { name: "左・通常", before: 83064, previous: 61912, after: 61784 },
  { name: "右・開発", before: 191956, previous: 159860, after: 159732 },
  { name: "左・開発", before: 93620, previous: 72468, after: 72340 },
];

export const reviewNotes = [
  {
    title: "view の size は入出力で意味が変わる",
    text: "read_view の呼び出し前は buffer 容量、成功後は payload 長。同じ変数を再利用する時は VIEW_BUFFER で初期化し直します。名前だけで借用とコピーを判断せず、呼び出す API の契約を確認してください。",
    status: "契約を API と図に明記",
  },
  {
    title: "value visitor と ref visitor を混同しない",
    text: "with_view の中から settings API を呼ぶと、共通 scratch の上書きや pool の移動で参照が壊れ得ます。ref_visit は識別子を一時 descriptor に解決する層なので、同期 settings API を呼べます。どちらも descriptor・借用 pointer の持ち出しや worker 待ちは避けます。",
    status: "公開宣言の近くに注意を配置",
  },
  {
    title: "state の型は descriptor だけでは決まらない",
    text: "通常の scalar は宣言型に対応する state。keyspace は公開上 INT32 や BEHAVIOR でも blob state です。array 要素は dense storage を使います。state accessor を無条件に呼ばず、それぞれの経路を確認します。",
    status: "例外を配置図に明記",
  },
  {
    title: "巨大な core と一般的すぎる private 名",
    text: "custom_settings.c には値の選択、検証、永続化、scope 操作が残っています。次の分割候補は永続化ですが、共有 scratch と lock の境界を先に固定する必要があります。copy_value / effective_value などの名前も private header を読まないと契約が伝わりにくい点は残ります。",
    status: "今後の改善候補・今回の動作変更なし",
  },
  {
    title: "互換 OFF の旧名は旧契約を意味しない",
    text: "struct tag の macro alias と API alias により重複実装はありませんが、OFF で旧 read 名を使っても出力 buffer が必要です。新しい呼び出し側では _view / _ref API を明示して移行を見える形にするのが読みやすい選択です。",
    status: "移行上の注意を明記",
  },
];
