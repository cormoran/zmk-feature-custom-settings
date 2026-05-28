import { useContext, useState } from "react";
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
  SettingScalarValue,
  SettingValue,
  SettingNotificationKind,
  SettingWriteMode,
} from "./proto/zmk/custom_settings/custom_settings";
import {
  createSettingsExportDocument,
  exportedSettingValueToProto,
  parseSettingsExportJson,
} from "./settingsJson";

export const SUBSYSTEM_IDENTIFIER = "zmk__custom_settings";
const LIST_NOTIFICATION_TIMEOUT_MS = 750;
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
  const [customSubsystemId, setCustomSubsystemId] = useState("test");
  const [settingKey, setSettingKey] = useState("int_value");
  const [keyPrefix, setKeyPrefix] = useState("");
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
  const [response, setResponse] = useState<string | null>(null);
  const [jsonText, setJsonText] = useState("");
  const [isLoading, setIsLoading] = useState(false);

  if (!zmkApp) return null;

  const subsystem = zmkApp.findSubsystem(SUBSYSTEM_IDENTIFIER);
  const settingSubsystem = zmkApp.findSubsystem(customSubsystemId);

  const settingRef = {
    customSubsystemIndex: settingSubsystem?.index,
    key: settingKey,
    source: 0,
    arrayIndex: isArray ? arrayIndex : undefined,
  };

  const settingScope = {
    customSubsystemIndex: settingSubsystem?.index,
    key: settingKey,
    keyPrefix,
    source: 0,
  };

  const parseScalarValue = (): SettingScalarValue => {
    switch (valueType) {
      case EditorValueType.Bytes:
        return { bytesValue: new TextEncoder().encode(value) };
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
    if (!zmkApp.state.connection || !subsystem) {
      throw new Error("Custom settings subsystem is not available");
    }

    const service = new ZMKCustomSubsystem(
      zmkApp.state.connection,
      subsystem.index
    );
    const payload = Request.encode(request).finish();
    const responsePayload = await service.callRPC(payload);
    if (!responsePayload) {
      throw new Error("Empty response");
    }

    return Response.decode(responsePayload);
  };

  const subsystemIdentifierForIndex = (index: number) =>
    zmkApp.state.customSubsystems?.subsystems[index]?.identifier;

  const subsystemIndexForIdentifier = (identifier: string) =>
    zmkApp.findSubsystem(identifier)?.index;

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

  const collectListSettings = async (): Promise<Setting[]> => {
    if (!subsystem) return [];

    const collected: Setting[] = [];
    let timeout: ReturnType<typeof setTimeout> | undefined;
    let resolveList: () => void = () => {};

    const listComplete = new Promise<void>((resolve) => {
      resolveList = resolve;
    });

    const scheduleResolve = () => {
      if (timeout) {
        clearTimeout(timeout);
      }
      timeout = setTimeout(resolveList, LIST_NOTIFICATION_TIMEOUT_MS);
    };

    const unsubscribe = zmkApp.onNotification({
      type: "custom",
      subsystemIndex: subsystem.index,
      callback: (customNotification) => {
        const notification = SettingsNotification.decode(
          customNotification.payload
        );
        if (
          notification.setting?.kind ===
            SettingNotificationKind.SETTING_NOTIFICATION_KIND_LIST_ITEM &&
          notification.setting.setting
        ) {
          collected.push(notification.setting.setting);
          scheduleResolve();
        }
      },
    });

    try {
      const resp = await callCustomRequest(
        Request.create({
          listSettings: {
            scope: {
              key: "",
              keyPrefix: "",
              source: SOURCE_ALL,
            },
          },
        })
      );
      if (resp.error) {
        throw new Error(resp.error.message || "List failed");
      }

      scheduleResolve();
      await listComplete;
    } finally {
      unsubscribe();
      if (timeout) {
        clearTimeout(timeout);
      }
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
      setResponse(`Exported ${exported.settings.length} setting values`);
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
                source: SOURCE_ALL,
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

  const listSettings = () =>
    sendRequest(
      Request.create({ listSettings: { scope: settingScope, requireMeta } })
    );

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

  const saveSettings = () =>
    sendRequest(Request.create({ saveSettings: { scope: settingScope } }));

  const discardSettings = () =>
    sendRequest(Request.create({ discardSettings: { scope: settingScope } }));

  const resetSettings = () =>
    sendRequest(Request.create({ resetSettings: { scope: settingScope } }));

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

  return (
    <section className="card">
      <h2>Settings</h2>

      <div className="form-grid">
        <label htmlFor="custom-subsystem-input">Custom Subsystem</label>
        <input
          id="custom-subsystem-input"
          value={customSubsystemId}
          onChange={(e) => setCustomSubsystemId(e.target.value)}
        />

        <label htmlFor="key-input">Key</label>
        <input
          id="key-input"
          value={settingKey}
          onChange={(e) => setSettingKey(e.target.value)}
        />

        <label htmlFor="prefix-input">Key Prefix</label>
        <input
          id="prefix-input"
          value={keyPrefix}
          onChange={(e) => setKeyPrefix(e.target.value)}
        />

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

        <label htmlFor="array-input">Array</label>
        <input
          id="array-input"
          type="checkbox"
          checked={isArray}
          onChange={(e) => setIsArray(e.target.checked)}
        />

        <label htmlFor="array-index-input">Array Index</label>
        <input
          id="array-index-input"
          type="number"
          min={0}
          value={arrayIndex}
          onChange={(e) =>
            setArrayIndex(Number.parseInt(e.target.value, 10) || 0)
          }
        />

        <label htmlFor="array-size-input">Array Size</label>
        <input
          id="array-size-input"
          type="number"
          min={1}
          value={arraySize}
          onChange={(e) =>
            setArraySize(Number.parseInt(e.target.value, 10) || 1)
          }
        />

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
        <button className="btn" disabled={isLoading} onClick={getSetting}>
          Read
        </button>
        <button
          className="btn btn-primary"
          disabled={isLoading}
          onClick={writeSetting}
        >
          Write
        </button>
        <button className="btn" disabled={isLoading} onClick={saveSettings}>
          Save
        </button>
        <button className="btn" disabled={isLoading} onClick={discardSettings}>
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

function formatScalarValue(value: SettingScalarValue): string {
  if (value.int32Value !== undefined) return `${value.int32Value}`;
  if (value.boolValue !== undefined) return value.boolValue ? "true" : "false";
  if (value.stringValue !== undefined) return value.stringValue;
  if (value.bytesValue !== undefined) {
    return Array.from(value.bytesValue)
      .map((byte) => byte.toString(16).padStart(2, "0"))
      .join(" ");
  }
  return "";
}

export default App;
