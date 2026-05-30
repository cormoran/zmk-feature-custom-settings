import { render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import {
  exportedSettingValueToProto,
  parseSettingsExportJson,
  settingsExportToJson,
  settingToExportedSetting,
} from "../src/settingsJson";
import { RPCTestSection, SUBSYSTEM_IDENTIFIER } from "../src/App";
import { Setting } from "../src/proto/zmk/custom_settings/custom_settings";

describe("RPCTestSection Component", () => {
  describe("With Subsystem", () => {
    it("should render RPC controls when subsystem is found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByRole("heading", { name: "Settings" })
      ).toBeInTheDocument();
      expect(screen.getByLabelText(/Custom Subsystem/i)).toBeInTheDocument();
      expect(screen.getByLabelText(/^Key$/i)).toBeInTheDocument();
      expect(screen.getByLabelText(/^Array$/i)).toBeInTheDocument();
      expect(screen.getByLabelText(/Array Index/i)).toBeInTheDocument();
      expect(screen.getByLabelText(/Settings JSON/i)).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Export JSON" })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Import JSON" })
      ).toBeInTheDocument();
      expect(screen.getByRole("button", { name: "Write" })).toBeInTheDocument();
    });

    it("should show default input value", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      const input = screen.getByLabelText(/Value/i) as HTMLInputElement;
      expect(input.value).toBe("10");
    });
  });

  describe("Without Subsystem", () => {
    it("should show warning when subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "zmk__custom_settings" not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(
          /Make sure your firmware includes the custom settings module/i
        )
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<RPCTestSection />);

      expect(container.firstChild).toBeNull();
    });
  });
});

describe("settings JSON conversion", () => {
  const baseSetting: Setting = {
    customSubsystemIndex: 1,
    key: "int_value",
    meta: undefined,
    hasUnsavedValue: false,
    value: { int32Value: 42 },
    source: 0,
  };

  it("exports scalar settings as typed JSON entries", () => {
    expect(settingToExportedSetting(baseSetting, () => "test")).toEqual({
      customSubsystemId: "test",
      key: "int_value",
      type: "int32",
      value: 42,
    });
  });

  it("exports array settings with public key and explicit index", () => {
    expect(
      settingToExportedSetting(
        {
          ...baseSetting,
          key: "array_value",
          value: {
            arrayValue: {
              index: 1,
              size: 2,
              maxSize: 3,
              value: { boolValue: true },
            },
          },
        },
        () => "test"
      )
    ).toEqual({
      customSubsystemId: "test",
      key: "array_value",
      type: "bool",
      value: true,
      arrayIndex: 1,
      arraySize: 2,
      arrayMaxSize: 3,
    });
  });

  it("parses exported JSON back to write values", () => {
    const json = settingsExportToJson([baseSetting], () => "test");
    const [setting] = parseSettingsExportJson(json);

    expect(setting).toMatchObject({
      customSubsystemId: "test",
      key: "int_value",
      type: "int32",
      value: 42,
    });
    expect(exportedSettingValueToProto(setting)).toEqual({ int32Value: 42 });
  });
});
