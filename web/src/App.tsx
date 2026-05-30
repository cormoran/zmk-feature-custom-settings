import { Fragment, useContext, useEffect, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Notification as SettingsNotification,
  Request,
  Response,
  Setting,
  SettingRef,
  SettingScope,
  SettingScalarValue,
  SettingValue,
  SettingNotificationKind,
  SettingWriteMode,
} from "./proto/cormoran/zmk/custom_settings/custom_settings";
import {
  createSettingsExportDocument,
  countExportedSettings,
  exportedSettingValueToProto,
  parseSettingsExportJson,
} from "./settingsJson";

export const SUBSYSTEM_IDENTIFIER = "cormoran_custom_settings";
const LIST_NOTIFICATION_TIMEOUT_MS = 750;
const LIST_REQUEST_TIMEOUT_MS = 5000;
const SOURCE_LOCAL = 0;
const SOURCE_ALL = 0xffffffff;

enum EditorValueType {
  Int32 = "int32",
  Bool = "bool",
  String = "string",
  Bytes = "bytes",
}

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>ZMK Custom Settings</h1>
        <p>Device settings console</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>{error}</p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <RPCTestSection />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>Settings module</strong>
        </p>
      </footer>
    </div>
  );
}

export function RPCTestSection() {
  const zmkApp = useContext(ZMKAppContext);
  const [filterCustomSubsystemId, setFilterCustomSubsystemId] = useState("");
  const [filterKey, setFilterKey] = useState("");
  const [filterKeyPrefix, setFilterKeyPrefix] = useState("");
  const [editorCustomSubsystemId, setEditorCustomSubsystemId] = useState("");
  const [editorSettingKey, setEditorSettingKey] = useState("");
  const [valueType, setValueType] = useState<EditorValueType>(
    EditorValueType.Int32
  );
  const [value, setValue] = useState("10");
  const [isArray, setIsArray] = useState(false);
  const [arrayIndex, setArrayIndex] = useState(0);
  const [arraySize, setArraySize] = useState(1);
  const [requireMeta, setRequireMeta] = useState(false);
  const [writeMode, setWriteMode] = useState<SettingWriteMode>(
    SettingWriteMode.SETTING_WRITE_MODE_MEMORY
  );
  const [setting, setSetting] = useState<Setting | null>(null);
  const [listedSettings, setListedSettings] = useState<Setting[]>([]);
  const [response, setResponse] = useState<string | null>(null);
  const [jsonText, setJsonText] = useState("");
  const [isLoading, setIsLoading] = useState(false);

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER) ?? undefined;
  const trimmedFilterCustomSubsystemId = filterCustomSubsystemId.trim();
  const trimmedEditorCustomSubsystemId = editorCustomSubsystemId.trim();
  const filterSubsystem = trimmedFilterCustomSubsystemId
    ? zmkApp?.findSubsystem(trimmedFilterCustomSubsystemId)
    : undefined;
  const editorSubsystem = trimmedEditorCustomSubsystemId
    ? zmkApp?.findSubsystem(trimmedEditorCustomSubsystemId)
    : undefined;

  const settingRef = {
    customSubsystemIndex: editorSubsystem?.index,
    key: editorSettingKey,
    source: setting?.source ?? SOURCE_LOCAL,
    arrayIndex: isArray ? arrayIndex : undefined,
  };

  const settingScope = {
    customSubsystemIndex: filterSubsystem?.index,
    key: optionalString(filterKey),
    keyPrefix: optionalString(filterKeyPrefix),
    source: SOURCE_ALL,
  };

  const listScope = {
    ...settingScope,
    source: SOURCE_ALL,
  };

  const parseScalarValue = (): SettingScalarValue => {
    switch (valueType) {
      case EditorValueType.Bytes:
        return { bytesValue: parseBytesValue(value) };
      case EditorValueType.Bool:
        return { boolValue: value === "true" || value === "1" };
      case EditorValueType.String:
        return { stringValue: value };
      case EditorValueType.Int32:
      default:
        return { int32Value: Number.parseInt(value, 10) || 0 };
    }
  };

  const parseValue = (): SettingValue => {
    const scalarValue = parseScalarValue();
    if (isArray) {
      return {
        arrayValue: {
          index: arrayIndex,
          size: arraySize,
          value: scalarValue,
        },
      };
    }

    return scalarValue;
  };

  const callCustomRequest = async (request: Request): Promise<Response> => {
    const connection = zmkApp?.state.connection;
    if (!connection || !subsystem) {
      throw new Error("Custom settings subsystem is not available");
    }

    const service = new ZMKCustomSubsystem(connection, subsystem.index);
    const payload = Request.encode(request).finish();
    console.debug("[custom-settings] RPC request", {
      request: requestSummary(request),
      payloadSize: payload.length,
      subsystemIndex: subsystem.index,
    });
    const responsePayload = await service.callRPC(payload);
    if (!responsePayload) {
      console.debug("[custom-settings] RPC empty response", {
        request: requestSummary(request),
      });
      throw new Error("Empty response");
    }

    const decoded = Response.decode(responsePayload);
    console.debug("[custom-settings] RPC response", {
      response: responseSummary(decoded),
      payloadSize: responsePayload.length,
    });
    return decoded;
  };

  const subsystemIdentifierForIndex = (index: number) =>
    zmkApp?.state.customSubsystems?.subsystems[index]?.identifier;

  const subsystemIndexForIdentifier = (identifier: string) =>
    zmkApp?.findSubsystem(identifier)?.index;

  const sendRequest = async (request: Request) => {
    setIsLoading(true);
    setResponse(null);

    try {
      const resp = await callCustomRequest(request);
      if (resp.getSetting?.setting) {
        setSetting(resp.getSetting.setting);
        setResponse(formatSetting(resp.getSetting.setting));
      } else if (resp.status) {
        setResponse(
          `${resp.status.message || "OK"} (${resp.status.affectedCount})`
        );
      } else if (resp.error) {
        setResponse(`Error: ${resp.error.message}`);
      }
    } catch (error) {
      console.error("RPC call failed:", error);
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const collectListSettings = async (
    requestScope: SettingScope = {
      source: SOURCE_ALL,
    },
    includeMeta = false
  ): Promise<Setting[]> => {
    if (!zmkApp || !subsystem) return [];

    console.debug("[custom-settings] list start", {
      scope: scopeSummary(requestScope),
      requireMeta: includeMeta,
    });
    const collected: Setting[] = [];
    const waitForQuiet = scopeTargetsAll(requestScope);
    let expectedCount: number | undefined;
    let quietTimeout: ReturnType<typeof setTimeout> | undefined;
    let resolveList: () => void = () => {};
    let isComplete = false;

    const listComplete = new Promise<void>((resolve) => {
      resolveList = resolve;
    });

    const completeList = (reason: string) => {
      if (isComplete) {
        return;
      }
      isComplete = true;
      if (quietTimeout) {
        clearTimeout(quietTimeout);
        quietTimeout = undefined;
      }
      console.debug("[custom-settings] list completion resolved", {
        collected: collected.length,
        expectedCount,
        reason,
      });
      resolveList();
    };

    const scheduleQuietResolve = () => {
      if (quietTimeout) {
        clearTimeout(quietTimeout);
      }
      quietTimeout = setTimeout(() => {
        console.debug("[custom-settings] list quiet timeout", {
          collected: collected.length,
          expectedCount,
          timeoutMs: LIST_NOTIFICATION_TIMEOUT_MS,
        });
        completeList("quiet-timeout");
      }, LIST_NOTIFICATION_TIMEOUT_MS);
    };

    const unsubscribe = zmkApp.onNotification({
      type: "custom",
      subsystemIndex: subsystem.index,
      callback: (customNotification) => {
        console.debug("[custom-settings] custom notification", {
          subsystemIndex: customNotification.subsystemIndex,
          payloadSize: customNotification.payload.length,
        });
        let notification;
        try {
          notification = SettingsNotification.decode(
            customNotification.payload
          );
        } catch (error) {
          console.debug("[custom-settings] notification decode failed", error);
          return;
        }
        console.debug(
          "[custom-settings] decoded notification",
          notificationSummary(notification)
        );
        if (
          notification.setting?.kind ===
            SettingNotificationKind.SETTING_NOTIFICATION_KIND_LIST_ITEM &&
          notification.setting.setting
        ) {
          collected.push(notification.setting.setting);
          if (
            expectedCount !== undefined &&
            !waitForQuiet &&
            collected.length >= expectedCount
          ) {
            completeList("expected-count");
          } else if (waitForQuiet) {
            scheduleQuietResolve();
          }
        }
      },
    });

    try {
      const resp = await withTimeout(
        callCustomRequest(
          Request.create({
            listSettings: {
              scope: requestScope,
              requireMeta: includeMeta,
            },
          })
        ),
        LIST_REQUEST_TIMEOUT_MS,
        "List request timed out"
      );
      if (resp.error) {
        throw new Error(resp.error.message || "List failed");
      }
      expectedCount = resp.status?.affectedCount;

      console.debug(
        "[custom-settings] list RPC accepted",
        responseSummary(resp)
      );
      if (!waitForQuiet && expectedCount !== undefined) {
        if (collected.length >= expectedCount) {
          completeList("expected-count");
        }
      } else {
        scheduleQuietResolve();
      }
      await listComplete;
    } finally {
      unsubscribe();
      if (quietTimeout) {
        clearTimeout(quietTimeout);
      }
      console.debug("[custom-settings] list complete", {
        collected: collected.length,
        expectedCount,
      });
    }

    return collected;
  };

  const exportJson = async () => {
    setIsLoading(true);
    setResponse(null);

    try {
      const settings = await collectListSettings();
      const exported = createSettingsExportDocument(
        settings,
        subsystemIdentifierForIndex
      );
      setJsonText(JSON.stringify(exported, null, 2));
      setResponse(`Exported ${countExportedSettings(exported)} setting values`);
    } catch (error) {
      console.error("Export failed:", error);
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const importJson = async () => {
    setIsLoading(true);
    setResponse(null);

    try {
      const settings = parseSettingsExportJson(jsonText);
      for (const importedSetting of settings) {
        const customSubsystemIndex = subsystemIndexForIdentifier(
          importedSetting.customSubsystemId
        );
        if (customSubsystemIndex === undefined) {
          throw new Error(
            `Custom subsystem not found: ${importedSetting.customSubsystemId}`
          );
        }

        const resp = await callCustomRequest(
          Request.create({
            writeSetting: {
              setting: {
                customSubsystemIndex,
                key: importedSetting.key,
                source: importedSetting.source,
                arrayIndex: importedSetting.arrayIndex,
              },
              value: exportedSettingValueToProto(importedSetting),
              mode: writeMode,
            },
          })
        );
        if (resp.error) {
          throw new Error(
            `${importedSetting.customSubsystemId}/${importedSetting.key}: ${resp.error.message}`
          );
        }
      }

      setResponse(`Imported ${settings.length} setting values`);
    } catch (error) {
      console.error("Import failed:", error);
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const getSetting = () =>
    sendRequest(
      Request.create({ getSetting: { setting: settingRef, requireMeta } })
    );

  const listSettings = async () => {
    setIsLoading(true);
    setResponse(null);

    try {
      if (trimmedFilterCustomSubsystemId && !filterSubsystem) {
        throw new Error(
          `Custom subsystem not found: ${trimmedFilterCustomSubsystemId}`
        );
      }

      const settings = await collectListSettings(listScope, requireMeta);
      setListedSettings(settings);
      setResponse(`Listed ${settings.length} settings`);
    } catch (error) {
      console.error("List failed:", error);
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const writeSetting = () =>
    sendRequest(
      Request.create({
        writeSetting: {
          setting: settingRef,
          value: parseValue(),
          mode: writeMode,
        },
      })
    );

  const pushBackArray = () =>
    sendRequest(
      Request.create({
        pushBackArray: {
          setting: settingRef,
          value: parseScalarValue(),
          mode: writeMode,
        },
      })
    );

  const popBackArray = () =>
    sendRequest(
      Request.create({
        popBackArray: {
          setting: settingRef,
          mode: writeMode,
        },
      })
    );

  const saveSettings = () =>
    sendRequest(Request.create({ saveSettings: { scope: settingScope } }));

  const discardSettings = () =>
    sendRequest(Request.create({ discardSettings: { scope: settingScope } }));

  const resetSettings = () =>
    sendRequest(Request.create({ resetSettings: { scope: settingScope } }));

  const selectSettingForEdit = (nextSetting: Setting) => {
    if (setting && settingsMatch(setting, nextSetting)) {
      setSetting(null);
      return;
    }

    setSetting(nextSetting);
    setEditorCustomSubsystemId(
      subsystemIdentifierForIndex(nextSetting.customSubsystemIndex) ??
        `${nextSetting.customSubsystemIndex}`
    );
    setEditorSettingKey(nextSetting.key);

    const arrayValue = nextSetting.value?.arrayValue;
    setIsArray(arrayValue !== undefined);
    if (arrayValue) {
      setArrayIndex(arrayValue.index);
      setArraySize(arrayValue.size);
      applyScalarValueToEditor(arrayValue.value ?? {});
    } else if (nextSetting.value) {
      applyScalarValueToEditor(nextSetting.value);
    } else {
      setValueType(EditorValueType.Int32);
      setValue("");
    }
  };

  const applyScalarValueToEditor = (nextValue: SettingScalarValue) => {
    if (nextValue.int32Value !== undefined) {
      setValueType(EditorValueType.Int32);
      setValue(`${nextValue.int32Value}`);
    } else if (nextValue.boolValue !== undefined) {
      setValueType(EditorValueType.Bool);
      setValue(nextValue.boolValue ? "true" : "false");
    } else if (nextValue.stringValue !== undefined) {
      setValueType(EditorValueType.String);
      setValue(nextValue.stringValue);
    } else if (nextValue.bytesValue !== undefined) {
      setValueType(EditorValueType.Bytes);
      setValue(formatBytesValue(nextValue.bytesValue));
    }
  };

  useEffect(() => {
    if (!subsystem) {
      return;
    }

    let cancelled = false;

    const loadAllSettings = async () => {
      setIsLoading(true);
      setResponse(null);

      try {
        const settings = await collectListSettings({ source: SOURCE_ALL });
        if (cancelled) {
          return;
        }
        setListedSettings(settings);
        setResponse(`Listed ${settings.length} settings`);
      } catch (error) {
        if (!cancelled) {
          console.error("Initial list failed:", error);
          setResponse(
            `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
          );
        }
      } finally {
        if (!cancelled) {
          setIsLoading(false);
        }
      }
    };

    void loadAllSettings();

    return () => {
      cancelled = true;
    };
    // Run once when the connected custom settings subsystem becomes available.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem?.index]);

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the custom settings module.
          </p>
        </div>
      </section>
    );
  }

  const selectedSettingIsArray = setting?.value?.arrayValue !== undefined;
  const selectedSettingDisplayName = setting
    ? settingDisplayName(setting, subsystemIdentifierForIndex)
    : "";
  const selectedSettingValueType = setting
    ? settingValueTypeLabel(setting.value)
    : undefined;
  const selectedSettingValueHidden =
    setting !== null && setting.value === undefined;
  const updateValuePanel = setting ? (
    <section className="settings-panel settings-editor-panel">
      <h3>Update Value</h3>
      <div className="form-grid">
        <span className="form-label">Setting</span>
        <span className="form-value">{selectedSettingDisplayName}</span>

        {selectedSettingValueType ? (
          <>
            <span className="form-label">Type</span>
            <span className="form-value">{selectedSettingValueType}</span>
          </>
        ) : (
          <>
            <label htmlFor="type-input">Type</label>
            <select
              id="type-input"
              value={valueType}
              onChange={(e) => setValueType(e.target.value as EditorValueType)}
            >
              <option value={EditorValueType.Int32}>int32</option>
              <option value={EditorValueType.Bool}>bool</option>
              <option value={EditorValueType.String}>string</option>
              <option value={EditorValueType.Bytes}>bytes</option>
            </select>
          </>
        )}

        {selectedSettingIsArray && (
          <>
            <span className="form-label">Array Index</span>
            <span className="form-value">{arrayIndex}</span>

            <span className="form-label">Array Size</span>
            <span className="form-value">{arraySize}</span>
          </>
        )}

        <label htmlFor="value-input">Value</label>
        <input
          id="value-input"
          value={value}
          onChange={(e) => setValue(e.target.value)}
        />

        <label htmlFor="mode-input">Write Mode</label>
        <select
          id="mode-input"
          value={writeMode}
          onChange={(e) => setWriteMode(Number(e.target.value))}
        >
          <option value={SettingWriteMode.SETTING_WRITE_MODE_MEMORY}>
            memory
          </option>
          <option value={SettingWriteMode.SETTING_WRITE_MODE_PERSIST}>
            persist
          </option>
        </select>
      </div>

      <div className="toolbar">
        <button className="btn" disabled={isLoading} onClick={getSetting}>
          Read
        </button>
        <button
          className="btn btn-primary"
          disabled={isLoading || selectedSettingValueHidden}
          onClick={writeSetting}
        >
          Write
        </button>
        {selectedSettingIsArray && (
          <>
            <button
              className="btn"
              disabled={isLoading || selectedSettingValueHidden}
              onClick={pushBackArray}
            >
              Push Back
            </button>
            <button className="btn" disabled={isLoading} onClick={popBackArray}>
              Pop Back
            </button>
          </>
        )}
      </div>
    </section>
  ) : null;

  return (
    <section className="card">
      <h2>Settings</h2>

      <section className="settings-panel">
        <h3>Filter</h3>
        <div className="form-grid">
          <label htmlFor="filter-custom-subsystem-input">
            Filter Subsystem
          </label>
          <input
            id="filter-custom-subsystem-input"
            value={filterCustomSubsystemId}
            onChange={(e) => setFilterCustomSubsystemId(e.target.value)}
          />

          <label htmlFor="filter-key-input">Filter Key</label>
          <input
            id="filter-key-input"
            value={filterKey}
            onChange={(e) => setFilterKey(e.target.value)}
          />

          <label htmlFor="filter-prefix-input">Key Prefix</label>
          <input
            id="filter-prefix-input"
            value={filterKeyPrefix}
            onChange={(e) => setFilterKeyPrefix(e.target.value)}
          />

          <label htmlFor="require-meta-input">Include Metadata</label>
          <input
            id="require-meta-input"
            type="checkbox"
            checked={requireMeta}
            onChange={(e) => setRequireMeta(e.target.checked)}
          />
        </div>

        <div className="toolbar">
          <button className="btn" disabled={isLoading} onClick={listSettings}>
            List
          </button>
          <button className="btn" disabled={isLoading} onClick={saveSettings}>
            Save
          </button>
          <button
            className="btn"
            disabled={isLoading}
            onClick={discardSettings}
          >
            Discard
          </button>
          <button
            className="btn btn-danger"
            disabled={isLoading}
            onClick={resetSettings}
          >
            Reset
          </button>
        </div>
      </section>

      <div className="settings-list">
        <div className="settings-list-header">
          <h3>Device Settings</h3>
          <span>{listedSettings.length} loaded</span>
        </div>
        {listedSettings.length > 0 ? (
          <div className="settings-table-wrap">
            <table className="settings-table">
              <thead>
                <tr>
                  <th>Setting</th>
                  <th>Value</th>
                  <th>Unsaved</th>
                  <th>Source</th>
                </tr>
              </thead>
              <tbody>
                {listedSettings.map((listedSetting, index) => {
                  const isSelected =
                    setting !== null && settingsMatch(setting, listedSetting);
                  return (
                    <Fragment key={settingRowKey(listedSetting, index)}>
                      <tr className={isSelected ? "selected-setting-row" : ""}>
                        <td>
                          <button
                            className="link-button"
                            type="button"
                            onClick={() => selectSettingForEdit(listedSetting)}
                          >
                            {settingDisplayName(
                              listedSetting,
                              subsystemIdentifierForIndex
                            )}
                          </button>
                        </td>
                        <td>
                          {listedSetting.value
                            ? formatValue(listedSetting.value)
                            : "(hidden)"}
                        </td>
                        <td>{listedSetting.hasUnsavedValue ? "yes" : "no"}</td>
                        <td>{formatSource(listedSetting.source)}</td>
                      </tr>
                      {isSelected && (
                        <tr className="settings-editor-row">
                          <td colSpan={4}>{updateValuePanel}</td>
                        </tr>
                      )}
                    </Fragment>
                  );
                })}
              </tbody>
            </table>
          </div>
        ) : (
          <p className="empty-message">
            Settings load automatically when the device connects. Use List to
            reload the current filter.
          </p>
        )}
      </div>

      <div className="json-panel">
        <label htmlFor="settings-json">Settings JSON</label>
        <textarea
          id="settings-json"
          rows={12}
          value={jsonText}
          onChange={(e) => setJsonText(e.target.value)}
        />
        <div className="toolbar">
          <button className="btn" disabled={isLoading} onClick={exportJson}>
            Export JSON
          </button>
          <button
            className="btn btn-primary"
            disabled={isLoading || jsonText.trim().length === 0}
            onClick={importJson}
          >
            Import JSON
          </button>
        </div>
      </div>

      {setting && (
        <dl className="setting-summary">
          <div>
            <dt>Setting</dt>
            <dd>
              {subsystemIdentifierForIndex(setting.customSubsystemIndex) ??
                setting.customSubsystemIndex}
              /{setting.key}
            </dd>
          </div>
          <div>
            <dt>Array</dt>
            <dd>
              {setting.value?.arrayValue
                ? `${setting.value.arrayValue.index}/${setting.value.arrayValue.size}`
                : "no"}
            </dd>
          </div>
          <div>
            <dt>Unsaved</dt>
            <dd>{setting.hasUnsavedValue ? "yes" : "no"}</dd>
          </div>
        </dl>
      )}

      {response && (
        <div className="response-box">
          <pre>{response}</pre>
        </div>
      )}
    </section>
  );
}

function formatSetting(setting: Setting): string {
  const value = setting.value ? formatValue(setting.value) : "(hidden)";
  return `${setting.customSubsystemIndex}/${setting.key} = ${value}`;
}

function formatValue(value: SettingValue): string {
  if (value.arrayValue !== undefined) {
    return `[${value.arrayValue.index}/${value.arrayValue.size}] ${formatScalarValue(value.arrayValue.value ?? {})}`;
  }

  return formatScalarValue(value);
}

function settingValueTypeLabel(
  value: SettingValue | undefined
): string | undefined {
  const scalarValue = value?.arrayValue?.value ?? value;
  if (!scalarValue) return undefined;
  if (scalarValue.int32Value !== undefined) return "int32";
  if (scalarValue.boolValue !== undefined) return "bool";
  if (scalarValue.stringValue !== undefined) return "string";
  if (scalarValue.bytesValue !== undefined) return "bytes";
  return undefined;
}

function formatScalarValue(value: SettingScalarValue): string {
  if (value.int32Value !== undefined) return `${value.int32Value}`;
  if (value.boolValue !== undefined) return value.boolValue ? "true" : "false";
  if (value.stringValue !== undefined) return value.stringValue;
  if (value.bytesValue !== undefined) {
    return formatBytesValue(value.bytesValue);
  }
  return "";
}

function parseBytesValue(value: string): Uint8Array {
  const trimmed = value.trim();
  if (trimmed.length === 0) return new Uint8Array();

  const tokens = trimmed.split(/[\s,]+/).filter(Boolean);
  if (
    tokens.length > 0 &&
    tokens.every((token) => /^[0-9a-fA-F]{1,2}$/.test(token))
  ) {
    return Uint8Array.from(tokens.map((token) => Number.parseInt(token, 16)));
  }

  return new TextEncoder().encode(value);
}

function formatBytesValue(value: Uint8Array): string {
  return Array.from(value)
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join(" ");
}

function settingRowKey(setting: Setting, index: number): string {
  const arrayIndex = setting.value?.arrayValue?.index ?? "scalar";
  return `${setting.source}:${setting.customSubsystemIndex}:${setting.key}:${arrayIndex}:${index}`;
}

function settingsMatch(a: Setting, b: Setting): boolean {
  return (
    a.source === b.source &&
    a.customSubsystemIndex === b.customSubsystemIndex &&
    a.key === b.key &&
    (a.value?.arrayValue?.index ?? undefined) ===
      (b.value?.arrayValue?.index ?? undefined)
  );
}

function settingDisplayName(
  setting: Setting,
  subsystemIdentifierForIndex: (index: number) => string | undefined
): string {
  const subsystem =
    subsystemIdentifierForIndex(setting.customSubsystemIndex) ??
    `${setting.customSubsystemIndex}`;
  const arraySuffix =
    setting.value?.arrayValue !== undefined
      ? `[${setting.value.arrayValue.index}]`
      : "";
  return `${subsystem}/${setting.key}${arraySuffix}`;
}

function formatSource(source: number): string {
  if (source === 0) return "local";
  if (source === SOURCE_ALL) return "all";
  return `${source}`;
}

function optionalString(value: string): string | undefined {
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}

function requestSummary(request: Request): Record<string, unknown> {
  if (request.listSettings) {
    return {
      kind: "listSettings",
      scope: scopeSummary(request.listSettings.scope),
      requireMeta: request.listSettings.requireMeta,
    };
  }
  if (request.getSetting) {
    return {
      kind: "getSetting",
      setting: refSummary(request.getSetting.setting),
      requireMeta: request.getSetting.requireMeta,
    };
  }
  if (request.writeSetting) {
    return {
      kind: "writeSetting",
      setting: refSummary(request.writeSetting.setting),
      valueKind: valueKind(request.writeSetting.value),
      mode: request.writeSetting.mode,
    };
  }
  if (request.saveSettings) {
    return {
      kind: "saveSettings",
      scope: scopeSummary(request.saveSettings.scope),
    };
  }
  if (request.discardSettings) {
    return {
      kind: "discardSettings",
      scope: scopeSummary(request.discardSettings.scope),
    };
  }
  if (request.resetSettings) {
    return {
      kind: "resetSettings",
      scope: scopeSummary(request.resetSettings.scope),
    };
  }
  if (request.pushBackArray) {
    return {
      kind: "pushBackArray",
      setting: refSummary(request.pushBackArray.setting),
      valueKind: scalarValueKind(request.pushBackArray.value),
      mode: request.pushBackArray.mode,
    };
  }
  if (request.popBackArray) {
    return {
      kind: "popBackArray",
      setting: refSummary(request.popBackArray.setting),
      mode: request.popBackArray.mode,
    };
  }

  return { kind: "unknown" };
}

function responseSummary(response: Response): Record<string, unknown> {
  if (response.error) {
    return { kind: "error", message: response.error.message };
  }
  if (response.status) {
    return {
      kind: "status",
      affectedCount: response.status.affectedCount,
      message: response.status.message,
    };
  }
  if (response.getSetting) {
    return {
      kind: "getSetting",
      setting: settingSummary(response.getSetting.setting),
    };
  }

  return { kind: "unknown" };
}

function notificationSummary(
  notification: SettingsNotification
): Record<string, unknown> {
  if (notification.setting) {
    return {
      kind: "setting",
      notificationKind: notification.setting.kind,
      setting: settingSummary(notification.setting.setting),
    };
  }

  return { kind: "unknown" };
}

function settingSummary(setting: Setting | undefined): Record<string, unknown> {
  if (!setting) {
    return { present: false };
  }

  return {
    present: true,
    customSubsystemIndex: setting.customSubsystemIndex,
    key: setting.key,
    source: setting.source,
    hasUnsavedValue: setting.hasUnsavedValue,
    hasMeta: setting.meta !== undefined,
    hasValue: setting.value !== undefined,
    valueKind: valueKind(setting.value),
  };
}

function refSummary(ref: SettingRef | undefined): Record<string, unknown> {
  if (!ref) {
    return { present: false };
  }

  return {
    present: true,
    customSubsystemIndex: ref.customSubsystemIndex,
    key: ref.key,
    source: ref.source,
    arrayIndex: ref.arrayIndex,
  };
}

function scopeSummary(
  scope: SettingScope | undefined
): Record<string, unknown> {
  if (!scope) {
    return { present: false };
  }

  return {
    present: true,
    customSubsystemIndex: scope.customSubsystemIndex,
    key: scope.key,
    keyPrefix: scope.keyPrefix,
    source: scope.source,
  };
}

function scopeTargetsAll(scope: SettingScope | undefined): boolean {
  return scope?.source === SOURCE_ALL;
}

async function withTimeout<T>(
  promise: Promise<T>,
  timeoutMs: number,
  message: string
): Promise<T> {
  let timeout: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      promise,
      new Promise<never>((_, reject) => {
        timeout = setTimeout(() => reject(new Error(message)), timeoutMs);
      }),
    ]);
  } finally {
    if (timeout) {
      clearTimeout(timeout);
    }
  }
}

function valueKind(value: SettingValue | undefined): string {
  if (!value) {
    return "none";
  }
  if (value.arrayValue) {
    return `array:${scalarValueKind(value.arrayValue.value)}`;
  }

  return scalarValueKind(value);
}

function scalarValueKind(value: SettingScalarValue | undefined): string {
  if (!value) {
    return "none";
  }
  if (value.bytesValue !== undefined) {
    return "bytes";
  }
  if (value.int32Value !== undefined) {
    return "int32";
  }
  if (value.boolValue !== undefined) {
    return "bool";
  }
  if (value.stringValue !== undefined) {
    return "string";
  }

  return "empty";
}

export default App;
