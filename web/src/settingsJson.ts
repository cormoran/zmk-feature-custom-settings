import {
  Setting,
  SettingScalarValue,
  SettingValue,
} from "./proto/zmk/custom_settings/custom_settings";

const SETTINGS_EXPORT_FORMAT = "zmk-custom-settings";
const SETTINGS_EXPORT_VERSION = 1;

type ExportedSettingType = "bytes" | "int32" | "bool" | "string";

export interface ExportedSetting {
  customSubsystemId: string;
  key: string;
  type: ExportedSettingType;
  value: boolean | number | string | number[];
  arrayIndex?: number;
  arraySize?: number;
  arrayMaxSize?: number;
}

export interface SettingsExportDocument {
  format: typeof SETTINGS_EXPORT_FORMAT;
  version: typeof SETTINGS_EXPORT_VERSION;
  exportedAt: string;
  settings: ExportedSetting[];
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
    exported.arrayMaxSize = setting.value.arrayValue.maxSize;
  }

  return exported;
}

export function createSettingsExportDocument(
  settings: Setting[],
  subsystemIdentifierForIndex?: (index: number) => string | undefined
): SettingsExportDocument {
  return {
    format: SETTINGS_EXPORT_FORMAT,
    version: SETTINGS_EXPORT_VERSION,
    exportedAt: new Date().toISOString(),
    settings: settings
      .map((setting) =>
        settingToExportedSetting(setting, subsystemIdentifierForIndex)
      )
      .filter((setting): setting is ExportedSetting => setting !== null),
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
  const settings = Array.isArray(parsed)
    ? parsed
    : isRecord(parsed) && Array.isArray(parsed.settings)
      ? parsed.settings
      : null;

  if (!settings) {
    throw new Error("JSON must contain a settings array");
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
        maxSize: setting.arrayMaxSize ?? setting.arraySize,
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
  if (!isRecord(entry)) {
    throw new Error(`settings[${index}] must be an object`);
  }

  const customSubsystemId = readString(entry, "customSubsystemId", index);
  const key = readString(entry, "key", index);
  const type = readSettingType(entry.type, index);
  const value = readSettingValue(entry.value, type, index);
  const exported: ExportedSetting = { customSubsystemId, key, type, value };

  if (entry.arrayIndex !== undefined) {
    exported.arrayIndex = readNonNegativeInteger(
      entry.arrayIndex,
      "arrayIndex",
      index
    );
    exported.arraySize = readPositiveInteger(
      entry.arraySize,
      "arraySize",
      index
    );
    if (entry.arrayMaxSize !== undefined) {
      exported.arrayMaxSize = readPositiveInteger(
        entry.arrayMaxSize,
        "arrayMaxSize",
        index
      );
    }
    if (exported.arrayIndex >= exported.arraySize) {
      throw new Error(
        `settings[${index}].arrayIndex must be smaller than arraySize`
      );
    }
    if (
      exported.arrayMaxSize !== undefined &&
      exported.arraySize > exported.arrayMaxSize
    ) {
      throw new Error(
        `settings[${index}].arraySize must be no larger than arrayMaxSize`
      );
    }
  }

  return exported;
}

function readString(
  entry: Record<string, unknown>,
  field: string,
  index: number
): string {
  const value = entry[field];
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`settings[${index}].${field} must be a non-empty string`);
  }

  return value;
}

function readSettingType(value: unknown, index: number): ExportedSettingType {
  if (
    value === "bytes" ||
    value === "int32" ||
    value === "bool" ||
    value === "string"
  ) {
    return value;
  }

  throw new Error(`settings[${index}].type is not supported`);
}

function readSettingValue(
  value: unknown,
  type: ExportedSettingType,
  index: number
): ExportedSetting["value"] {
  switch (type) {
    case "bytes":
      if (
        !Array.isArray(value) ||
        value.some((byte) => !Number.isInteger(byte) || byte < 0 || byte > 255)
      ) {
        throw new Error(`settings[${index}].value must be a byte array`);
      }
      return value as number[];
    case "int32":
      if (!Number.isInteger(value)) {
        throw new Error(`settings[${index}].value must be an integer`);
      }
      return value as number;
    case "bool":
      if (typeof value !== "boolean") {
        throw new Error(`settings[${index}].value must be a boolean`);
      }
      return value;
    case "string":
      if (typeof value !== "string") {
        throw new Error(`settings[${index}].value must be a string`);
      }
      return value;
  }
}

function readNonNegativeInteger(
  value: unknown,
  field: string,
  index: number
): number {
  if (typeof value !== "number" || !Number.isInteger(value) || value < 0) {
    throw new Error(
      `settings[${index}].${field} must be a non-negative integer`
    );
  }

  return value;
}

function readPositiveInteger(
  value: unknown,
  field: string,
  index: number
): number {
  if (typeof value !== "number" || !Number.isInteger(value) || value <= 0) {
    throw new Error(`settings[${index}].${field} must be a positive integer`);
  }

  return value;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
