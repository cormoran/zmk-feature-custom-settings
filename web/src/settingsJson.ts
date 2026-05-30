import {
  Setting,
  SettingScalarValue,
  SettingValue,
} from "./proto/zmk/custom_settings/custom_settings";

const SETTINGS_EXPORT_FORMAT = "zmk-custom-settings";
const SETTINGS_EXPORT_VERSION = 2;

type ExportedSettingType = "bytes" | "int32" | "bool" | "string";

export interface ExportedSetting {
  customSubsystemId: string;
  key: string;
  type: ExportedSettingType;
  value: boolean | number | string | number[];
  arrayIndex?: number;
  arraySize?: number;
}

export type ExportedSubsystemSetting = Omit<
  ExportedSetting,
  "customSubsystemId"
>;

export interface SettingsExportDocument {
  format: typeof SETTINGS_EXPORT_FORMAT;
  version: typeof SETTINGS_EXPORT_VERSION;
  exportedAt: string;
  customSubsystems: Record<string, ExportedSubsystemSetting[]>;
}

export function countExportedSettings(doc: SettingsExportDocument): number {
  return Object.values(doc.customSubsystems).reduce(
    (sum, settings) => sum + settings.length,
    0
  );
}

export function settingToExportedSetting(
  setting: Setting,
  subsystemIdentifierForIndex: (index: number) => string | undefined = (
    index
  ) => String(index)
): ExportedSetting | null {
  if (!setting.value) {
    return null;
  }

  const scalarValue = setting.value.arrayValue?.value ?? setting.value;
  const type = scalarValueType(scalarValue);
  if (!type) {
    return null;
  }

  const value = scalarValueToJsonValue(type, scalarValue);
  if (value === undefined) {
    return null;
  }

  const customSubsystemId = subsystemIdentifierForIndex(
    setting.customSubsystemIndex
  );
  if (!customSubsystemId) {
    return null;
  }

  const exported: ExportedSetting = {
    customSubsystemId,
    key: setting.key,
    type,
    value,
  };

  if (setting.value.arrayValue) {
    exported.arrayIndex = setting.value.arrayValue.index;
    exported.arraySize = setting.value.arrayValue.size;
  }

  return exported;
}

export function createSettingsExportDocument(
  settings: Setting[],
  subsystemIdentifierForIndex?: (index: number) => string | undefined
): SettingsExportDocument {
  const customSubsystems: Record<string, ExportedSubsystemSetting[]> = {};

  for (const setting of settings) {
    const exported = settingToExportedSetting(
      setting,
      subsystemIdentifierForIndex
    );
    if (!exported) continue;

    const { customSubsystemId, ...subsystemSetting } = exported;
    if (!customSubsystems[customSubsystemId]) {
      customSubsystems[customSubsystemId] = [];
    }
    customSubsystems[customSubsystemId].push(subsystemSetting);
  }

  return {
    format: SETTINGS_EXPORT_FORMAT,
    version: SETTINGS_EXPORT_VERSION,
    exportedAt: new Date().toISOString(),
    customSubsystems,
  };
}

export function settingsExportToJson(
  settings: Setting[],
  subsystemIdentifierForIndex?: (index: number) => string | undefined
): string {
  return JSON.stringify(
    createSettingsExportDocument(settings, subsystemIdentifierForIndex),
    null,
    2
  );
}

export function parseSettingsExportJson(json: string): ExportedSetting[] {
  const parsed: unknown = JSON.parse(json);

  // New format: { customSubsystems: { [id]: [...] } }
  if (isRecord(parsed) && isRecord(parsed.customSubsystems)) {
    const result: ExportedSetting[] = [];
    for (const [customSubsystemId, subsystemSettings] of Object.entries(
      parsed.customSubsystems
    )) {
      if (!Array.isArray(subsystemSettings)) {
        throw new Error(
          `customSubsystems.${customSubsystemId} must be an array`
        );
      }
      for (let i = 0; i < subsystemSettings.length; i++) {
        result.push(
          parseSubsystemSetting(subsystemSettings[i], customSubsystemId, i)
        );
      }
    }
    return result;
  }

  // Legacy format: flat array or { settings: [...] }
  const settings = Array.isArray(parsed)
    ? parsed
    : isRecord(parsed) && Array.isArray(parsed.settings)
      ? parsed.settings
      : null;

  if (!settings) {
    throw new Error(
      "JSON must contain a customSubsystems object or settings array"
    );
  }

  return settings.map((entry, index) => parseExportedSetting(entry, index));
}

export function exportedSettingValueToProto(
  setting: ExportedSetting
): SettingValue {
  const scalarValue = exportedScalarValueToProto(setting);
  if (setting.arrayIndex !== undefined) {
    if (setting.arraySize === undefined) {
      throw new Error(
        `${setting.customSubsystemId}/${setting.key} is missing arraySize`
      );
    }

    return {
      arrayValue: {
        index: setting.arrayIndex,
        size: setting.arraySize,
        value: scalarValue,
      },
    };
  }

  return scalarValue;
}

function scalarValueType(
  value: SettingScalarValue
): ExportedSettingType | null {
  if (value.bytesValue !== undefined) return "bytes";
  if (value.int32Value !== undefined) return "int32";
  if (value.boolValue !== undefined) return "bool";
  if (value.stringValue !== undefined) return "string";
  return null;
}

function scalarValueToJsonValue(
  type: ExportedSettingType,
  value: SettingScalarValue
): ExportedSetting["value"] | undefined {
  switch (type) {
    case "bytes":
      return value.bytesValue ? Array.from(value.bytesValue) : undefined;
    case "int32":
      return value.int32Value;
    case "bool":
      return value.boolValue;
    case "string":
      return value.stringValue;
  }
}

function exportedScalarValueToProto(setting: ExportedSetting): SettingValue {
  switch (setting.type) {
    case "bytes":
      if (!Array.isArray(setting.value)) {
        throw new Error(
          `${setting.customSubsystemId}/${setting.key} bytes value must be an array`
        );
      }
      return { bytesValue: Uint8Array.from(setting.value) };
    case "int32":
      if (typeof setting.value !== "number") {
        throw new Error(
          `${setting.customSubsystemId}/${setting.key} int32 value must be a number`
        );
      }
      return { int32Value: setting.value };
    case "bool":
      if (typeof setting.value !== "boolean") {
        throw new Error(
          `${setting.customSubsystemId}/${setting.key} bool value must be a boolean`
        );
      }
      return { boolValue: setting.value };
    case "string":
      if (typeof setting.value !== "string") {
        throw new Error(
          `${setting.customSubsystemId}/${setting.key} string value must be a string`
        );
      }
      return { stringValue: setting.value };
  }
}

function parseExportedSetting(entry: unknown, index: number): ExportedSetting {
  const label = `settings[${index}]`;
  if (!isRecord(entry)) {
    throw new Error(`${label} must be an object`);
  }

  const customSubsystemId = readString(entry, "customSubsystemId", label);
  return parseSubsystemSetting(entry, customSubsystemId, label);
}

function parseSubsystemSetting(
  entry: unknown,
  customSubsystemId: string,
  index: number | string
): ExportedSetting {
  const label =
    typeof index === "string"
      ? index
      : `customSubsystems.${customSubsystemId}[${index}]`;
  if (!isRecord(entry)) {
    throw new Error(`${label} must be an object`);
  }

  const key = readString(entry, "key", label);
  const type = readSettingType(entry.type, label);
  const value = readSettingValue(entry.value, type, label);
  const exported: ExportedSetting = { customSubsystemId, key, type, value };

  if (entry.arrayIndex !== undefined) {
    exported.arrayIndex = readNonNegativeInteger(
      entry.arrayIndex,
      "arrayIndex",
      label
    );
    exported.arraySize = readPositiveInteger(
      entry.arraySize,
      "arraySize",
      label
    );
    if (exported.arrayIndex >= exported.arraySize) {
      throw new Error(`${label}.arrayIndex must be smaller than arraySize`);
    }
  }

  return exported;
}

function readString(
  entry: Record<string, unknown>,
  field: string,
  label: string
): string {
  const value = entry[field];
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`${label}.${field} must be a non-empty string`);
  }

  return value;
}

function readSettingType(value: unknown, label: string): ExportedSettingType {
  if (
    value === "bytes" ||
    value === "int32" ||
    value === "bool" ||
    value === "string"
  ) {
    return value;
  }

  throw new Error(`${label}.type is not supported`);
}

function readSettingValue(
  value: unknown,
  type: ExportedSettingType,
  label: string
): ExportedSetting["value"] {
  switch (type) {
    case "bytes":
      if (
        !Array.isArray(value) ||
        value.some((byte) => !Number.isInteger(byte) || byte < 0 || byte > 255)
      ) {
        throw new Error(`${label}.value must be a byte array`);
      }
      return value as number[];
    case "int32":
      if (!Number.isInteger(value)) {
        throw new Error(`${label}.value must be an integer`);
      }
      return value as number;
    case "bool":
      if (typeof value !== "boolean") {
        throw new Error(`${label}.value must be a boolean`);
      }
      return value;
    case "string":
      if (typeof value !== "string") {
        throw new Error(`${label}.value must be a string`);
      }
      return value;
  }
}

function readNonNegativeInteger(
  value: unknown,
  field: string,
  label: string
): number {
  if (typeof value !== "number" || !Number.isInteger(value) || value < 0) {
    throw new Error(`${label}.${field} must be a non-negative integer`);
  }

  return value;
}

function readPositiveInteger(
  value: unknown,
  field: string,
  label: string
): number {
  if (typeof value !== "number" || !Number.isInteger(value) || value <= 0) {
    throw new Error(`${label}.${field} must be a positive integer`);
  }

  return value;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
