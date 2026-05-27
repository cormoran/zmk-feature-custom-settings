import { useContext, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  Setting,
  SettingScalarValue,
  SettingValue,
  SettingValueType,
  SettingWriteMode,
} from "./proto/zmk/custom_settings/custom_settings";

export const SUBSYSTEM_IDENTIFIER = "zmk__custom_settings";

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
  const [subsystemId, setSubsystemId] = useState("test");
  const [settingKey, setSettingKey] = useState("int_value");
  const [keyPrefix, setKeyPrefix] = useState("");
  const [valueType, setValueType] = useState<SettingValueType>(
    SettingValueType.SETTING_VALUE_TYPE_INT32
  );
  const [value, setValue] = useState("10");
  const [isArray, setIsArray] = useState(false);
  const [arrayIndex, setArrayIndex] = useState(0);
  const [arraySize, setArraySize] = useState(1);
  const [writeMode, setWriteMode] = useState<SettingWriteMode>(
    SettingWriteMode.SETTING_WRITE_MODE_MEMORY
  );
  const [setting, setSetting] = useState<Setting | null>(null);
  const [response, setResponse] = useState<string | null>(null);
  const [isLoading, setIsLoading] = useState(false);

  if (!zmkApp) return null;

  const subsystem = zmkApp.findSubsystem(SUBSYSTEM_IDENTIFIER);

  const settingRef = {
    subsystemId,
    key: settingKey,
    source: 0,
    arrayIndex,
    hasArrayIndex: isArray,
  };

  const settingScope = {
    subsystemId,
    key: settingKey,
    keyPrefix,
    source: 0,
  };

  const parseScalarValue = (): SettingScalarValue => {
    switch (valueType) {
      case SettingValueType.SETTING_VALUE_TYPE_BYTES:
        return { bytesValue: new TextEncoder().encode(value) };
      case SettingValueType.SETTING_VALUE_TYPE_BOOL:
        return { boolValue: value === "true" || value === "1" };
      case SettingValueType.SETTING_VALUE_TYPE_STRING:
        return { stringValue: value };
      case SettingValueType.SETTING_VALUE_TYPE_INT32:
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

  const sendRequest = async (request: Request) => {
    if (!zmkApp.state.connection || !subsystem) return;

    setIsLoading(true);
    setResponse(null);

    try {
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );
      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);

      if (responsePayload) {
        const resp = Response.decode(responsePayload);
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

  const getSetting = () =>
    sendRequest(Request.create({ getSetting: { setting: settingRef } }));

  const listSettings = () =>
    sendRequest(Request.create({ listSettings: { scope: settingScope } }));

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
        <label htmlFor="subsystem-input">Subsystem</label>
        <input
          id="subsystem-input"
          value={subsystemId}
          onChange={(e) => setSubsystemId(e.target.value)}
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
          onChange={(e) => setValueType(Number(e.target.value))}
        >
          <option value={SettingValueType.SETTING_VALUE_TYPE_INT32}>
            int32
          </option>
          <option value={SettingValueType.SETTING_VALUE_TYPE_BOOL}>bool</option>
          <option value={SettingValueType.SETTING_VALUE_TYPE_STRING}>
            string
          </option>
          <option value={SettingValueType.SETTING_VALUE_TYPE_BYTES}>
            bytes
          </option>
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

      {setting && (
        <dl className="setting-summary">
          <div>
            <dt>Setting</dt>
            <dd>
              {setting.subsystemId}/{setting.key}
            </dd>
          </div>
          <div>
            <dt>Array</dt>
            <dd>
              {setting.isArray
                ? `${setting.arrayIndex}/${setting.arraySize}`
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
  return `${setting.subsystemId}/${setting.key} = ${value}`;
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
