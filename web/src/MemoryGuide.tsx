import { useState } from "react";
import "./MemoryGuide.css";

const repository = "https://github.com/cormoran/zmk-feature-custom-settings";

function Flow({ steps, label }: { steps: string[]; label: string }) {
  return (
    <figure className="memory-flow">
      <ol aria-label={label}>
        {steps.map((step) => (
          <li key={step}>{step}</li>
        ))}
      </ol>
      <figcaption>{label}</figcaption>
    </figure>
  );
}

function PoolExample() {
  const [sizes, setSizes] = useState([96, 64, 0]);
  const [strings, setStrings] = useState(false);
  const used = sizes.reduce((sum, size) => sum + size + (strings ? 1 : 0), 0);
  const capacity = 256;
  const fits = used <= capacity;
  return (
    <div className="pool-example">
      <h3>Try a shared 256-byte pool</h3>
      <p>
        Three settings, each with max_size = 128. Sliders propose payload
        lengths in bytes; STRING adds one NUL byte per setting.
      </p>
      <label className="pool-type">
        <input
          type="checkbox"
          checked={strings}
          onChange={(event) => setStrings(event.target.checked)}
        />{" "}
        Use STRING instead of BYTES
      </label>
      <div className="pool-controls">
        {sizes.map((size, index) => (
          <label key={index}>
            Setting {String.fromCharCode(65 + index)}: {size} bytes
            <input
              type="range"
              min="0"
              max="128"
              value={size}
              onChange={(event) =>
                setSizes(
                  sizes.map((old, position) =>
                    position === index ? Number(event.target.value) : old
                  )
                )
              }
            />
          </label>
        ))}
      </div>
      <figure>
        <div
          className="pool-bar"
          role="img"
          aria-label={`Proposed pool usage: ${used} of ${capacity} bytes${fits ? "" : ", exceeds capacity"}`}
        >
          {sizes.map((size, index) => {
            const bytes = size + (strings ? 1 : 0);
            return (
              bytes > 0 && (
                <span
                  key={index}
                  className={`pool-member member-${index}`}
                  style={{ flex: bytes }}
                >
                  {String.fromCharCode(65 + index)}
                </span>
              )
            );
          })}
          {fits && used < capacity && (
            <span className="pool-free" style={{ flex: capacity - used }}>
              Free
            </span>
          )}
        </div>
        <figcaption>
          A / B / C = proposed regions.{" "}
          {fits
            ? "Unused capacity stays reserved in RAM."
            : "Overflow preview only; this layout cannot be committed."}
        </figcaption>
      </figure>
      <p
        role="status"
        className={fits ? "pool-result" : "pool-result pool-error"}
      >
        {fits
          ? `${used} / 256 bytes occupied · ${capacity - used} bytes available inside the pool.`
          : `${used} / 256 bytes requested · write fails with -ENOSPC; the previous value stays unchanged.`}
      </p>
      <p>
        <strong>Reserved backing RAM: always 256 bytes</strong>, plus three
        state blocks and a pool descriptor. Emptying values makes room for other
        members; it does not return RAM to the system. Regions compact on
        writes. Diagrams show byte storage only, not bookkeeping.
      </p>
      <button
        type="button"
        onClick={() => {
          setSizes([0, 0, 0]);
          setStrings(false);
        }}
      >
        Empty all BYTES values
      </button>
    </div>
  );
}

export default function MemoryGuide() {
  return (
    <main className="memory-guide">
      <header>
        <a href="./">← Device settings console</a>
        <p className="eyebrow">ZMK Custom Settings · Illustrated guide</p>
        <h1>Where do your settings live?</h1>
        <p className="memory-intro">
          Follow a value from its flash default to RAM, through a shared pool,
          and back to persistent storage.
        </p>
        <p>
          No keyboard connection required. These are conceptual diagrams of the
          module, not live device measurements. Exact totals depend on
          architecture, configuration, alignment, and enabled features.
        </p>
      </header>
      <nav aria-label="Guide sections">
        <a href="#overview">Flash & RAM</a>
        <a href="#values">Scalar values</a>
        <a href="#pools">Large pools</a>
        <a href="#collections">Arrays & keyspaces</a>
        <a href="#lifecycle">Write & save</a>
        <a href="#rpc">RPC & scratch space</a>
      </nav>

      <section id="overview">
        <h2>01 · Three places, three jobs</h2>
        <div className="memory-columns">
          <article className="memory-box flash">
            <h3>Firmware flash</h3>
            <p>Const registration descriptor</p>
            <p>Identity · type · permissions · constraints</p>
            <p>Compile-time default ← referenced by pointer</p>
          </article>
          <article className="memory-box ram">
            <h3>RAM</h3>
            <p>Mutable state + current value</p>
            <p>Shared temporary slots / pools / scratch buffers</p>
            <p>Lost on power-off</p>
          </article>
          <article className="memory-box saved">
            <h3>Settings flash</h3>
            <p>Persisted records</p>
            <p>Written by Save / PERSIST</p>
            <p>Survive power-off; loaded at boot</p>
          </article>
        </div>
        <Flow
          steps={[
            "Boot / Discard",
            "Saved record OR current default if absent",
            "Restore RAM value",
          ]}
          label="Restoration reads flash. There is no separate RAM cache of the saved value."
        />
        <aside>
          Static reservation ≠ current occupancy. All declared backing buffers
          and pools reserve RAM at build time. “Allocated on demand” means a
          region inside that already-reserved pool, without a heap allocation.
        </aside>
      </section>

      <section id="values">
        <h2>02 · What each scalar setting adds</h2>
        <p>
          A static scalar descriptor and its compile-time default live in flash.
          Each setting also has a compact mutable state block (about 24 bytes on
          ARM32; configuration-dependent).
        </p>
        <div className="memory-columns">
          <article className="memory-box ram">
            <h3>INT32 / BOOL / BEHAVIOR</h3>
            <div className="memory-cell">
              State <strong>including inline value</strong>
            </div>
            <p>
              No separate value buffer. A BOOL uses about 24 bytes total, not a
              full exchange carrier.
            </p>
          </article>
          <article className="memory-box ram">
            <h3>Plain BYTES / STRING</h3>
            <div className="memory-cell">State → static value buffer</div>
            <p>
              <code>VALUE_MAX_SIZE + 1</code> bytes per buffer (65 at the
              default 64), even for an empty value. The extra byte allows a
              string terminator.
            </p>
          </article>
          <article className="memory-box ram">
            <h3>SIZED / POOLED</h3>
            <div className="memory-cell">State → region in a pool</div>
            <p>
              Region follows actual payload length. Empty BYTES uses 0 pool
              bytes; empty STRING still uses 1 for NUL.
            </p>
          </article>
        </div>
        <p>
          Increasing <code>CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE</code> does
          not enlarge inline scalar values. It does size plain byte/string
          buffers and the fixed API exchange carrier.
        </p>
      </section>

      <section id="pools">
        <h2>03 · Large values share a fixed budget</h2>
        <Flow
          steps={[
            "DEFINE_SIZED",
            "Private pool: max_size + 1 bytes",
            "One setting owns the capacity",
          ]}
          label="Guaranteed backing capacity for one large value, plus pool bookkeeping."
        />
        <Flow
          steps={[
            "LARGE_POOL_DEFINE + DEFINE_POOLED",
            "One explicit byte budget",
            "Multiple settings share capacity",
          ]}
          label="Sharing saves reserved RAM when members do not all need their maximum at once."
        />
        <PoolExample />
        <p>
          Each setting must also fit its own <code>max_size</code>, bounded by{" "}
          <code>CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE</code>. If a
          saved value no longer fits after shrinking a pool in a firmware
          update, loading skips it with a warning.
        </p>
      </section>

      <section id="collections">
        <h2>04 · Collections have two different layouts</h2>
        <h3>Array: one registration, one contiguous buffer</h3>
        <figure>
          <div className="memory-cell flash">
            One const descriptor + defaults in flash
          </div>
          <div className="memory-cell ram">
            RAM: array state → backing buffer sized for maximum count
          </div>
          <div
            className="array-cells"
            role="img"
            aria-label="Example array: 3 active elements and 2 inactive elements, all 5 reserved in RAM"
          >
            <span>0 · active</span>
            <span>1 · active</span>
            <span>2 · active</span>
            <span className="pool-free">3 · inactive</span>
            <span className="pool-free">4 · inactive</span>
          </div>
          <figcaption>
            Example: active length 3, maximum 5. Popping an item does not shrink
            the backing allocation. Saving stores the active length and active
            items only.
          </figcaption>
        </figure>
        <p>
          Element access uses shared cached index views (
          <code>CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE</code>, default
          16). That pool is extra shared RAM; it does not set the maximum array
          length.
        </p>
        <h3>Keyspace: fixed slots + a shared key/value pool</h3>
        <Flow
          steps={[
            "Reserved slot array (max_entries)",
            "Live slot points into byte pool",
            "[key + NUL][value payload]",
          ]}
          label="User-created keys occupy both a slot and a byte region; no heap is used."
        />
        <p>
          The default pool reserves{" "}
          <code>max_entries × (max_key_len + max_size)</code> bytes, in addition
          to slot metadata. For 8 slots, a 16-byte key budget (including NUL)
          and 4-byte INT32 payload, that is{" "}
          <strong>160 bytes of pool backing</strong>, not 160 bytes total RAM.
        </p>
        <p>
          <code>DEFINE_WITH_POOL_SIZE</code> can set a smaller shared budget.
          Create fails with <code>-ENOSPC</code> if slots or bytes run out.
          Deleting an entry frees its slot and region for reuse; the declared
          RAM remains reserved.
        </p>
      </section>

      <section id="lifecycle">
        <h2>05 · Temporary, current, and saved are different</h2>
        <Flow
          steps={[
            "TEMPORARY slot if active; otherwise current RAM value",
            "Effective value returned to readers",
          ]}
          label="Read precedence: an override hides the current value without replacing it."
        />
        <div className="memory-columns">
          <article className="memory-box ram">
            <h3>Shared override slots</h3>
            <div className="memory-cell">Slot 0 → setting A</div>
            <div className="memory-cell">Slot 1 → setting B</div>
            <p>
              Default: 2 slots × 32 payload bytes, plus metadata and shared
              reconstruction scratch. This is not a slot for every setting.
            </p>
            <p>
              Too large: <code>-EMSGSIZE</code>. No free slot:{" "}
              <code>-EBUSY</code>. Rollback reveals the underlying RAM value.
            </p>
          </article>
          <article className="memory-box saved">
            <h3>Current → saved → restored</h3>
            <dl>
              <dt>MEMORY</dt>
              <dd>Change current RAM value; mark unsaved.</dd>
              <dt>Save / PERSIST</dt>
              <dd>Persist current / supplied value to settings flash.</dd>
              <dt>Discard</dt>
              <dd>
                Clear unsaved/temporary changes and reload saved value or
                default from flash.
              </dd>
            </dl>
          </article>
        </div>
        <p>
          For static settings, saving a value equal to its default deletes the
          stored record. The next boot can then follow a new firmware default.
          Keyspace entries are exempt: their user-defined keys must be
          persisted.
        </p>
        <p>
          The unsaved indicator is a flag, not a flash comparison: writing the
          already-saved value still marks it unsaved until save/discard/reset.
        </p>
      </section>

      <section id="rpc">
        <h2>06 · Moving a value also needs memory</h2>
        <Flow
          steps={[
            "Browser sends chunks",
            "One shared write staging buffer",
            "Validate on commit",
            "Apply to setting storage",
          ]}
          label="Chunked RPC writes stage one transfer at a time; partial values are not visible to readers."
        />
        <p>
          <code>CONFIG_ZMK_CUSTOM_SETTINGS_CHUNK_STAGING_SIZE</code> controls
          that buffer (default: <code>LARGE_VALUE_MAX_SIZE</code>). Staging and
          the destination pool coexist in RAM. A new transfer at offset 0
          replaces an abandoned transfer.
        </p>
        <Flow
          steps={[
            "Setting storage",
            "GetSetting / ListSettings encoder",
            "Transport TX chunks",
            "Browser",
          ]}
          label="Reads stream the response; the TX buffer size is not a maximum response length."
        />
        <div className="memory-columns">
          <article className="memory-box">
            <h3>Firmware API / stack</h3>
            <p>
              <code>struct zmk_custom_setting_value</code> carries up to{" "}
              <code>VALUE_MAX_SIZE</code> plus metadata. A local variable costs
              stack space even for BOOL. Typed getters/setters,{" "}
              <code>read_into</code>, and borrowed views avoid a full carrier
              where appropriate.
            </p>
          </article>
          <article className="memory-box">
            <h3>Records & optional features</h3>
            <p>
              A record encodes fields into an underlying BYTES setting; it uses
              that setting’s storage policy. Each record_get / record_set call
              uses a LARGE_VALUE_MAX_SIZE-byte encoded buffer on the stack, in
              addition to the caller’s struct. Raising that limit increases peak
              stack demand. RPC handlers also have shared decode buffers; those
              are not multiplied by the number of settings.
            </p>
            <p>
              Disabling unused ARRAY, KEYSPACE, LARGE_VALUES, RECORD,
              RPC_CONVERTERS, or CHUNKED_RPC features removes their associated
              code and/or RAM costs.
            </p>
          </article>
        </div>
        <aside>
          Split keyboards: large values are effectively local-only. The split
          relay has a fixed envelope and no chunked-write path; a peripheral
          notification omits a value that does not fit. Connect directly to the
          half that owns a large value.
        </aside>
      </section>
      <footer>
        <h2>Check your actual firmware budget</h2>
        <p>
          Add per-setting state and backing storage, shared pools and scratch
          buffers, and stack/transport costs. These diagrams are not a
          whole-firmware RAM calculator; use your build’s RAM/flash report for
          totals.
        </p>
        <p>
          Sources:{" "}
          <a href={`${repository}/blob/main/README.md#memory-notes`}>
            README memory notes
          </a>{" "}
          ·{" "}
          <a
            href={`${repository}/blob/main/include/cormoran/zmk/custom_settings.h`}
          >
            Storage declarations
          </a>{" "}
          ·{" "}
          <a href={`${repository}/blob/main/Kconfig`}>
            Kconfig limits and defaults
          </a>
        </p>
        <a href="./">← Back to settings</a>
      </footer>
    </main>
  );
}
