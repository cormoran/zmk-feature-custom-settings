import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import {
  exportedSettingValueToProto,
  parseSettingsExportJson,
  settingsExportToJson,
  settingToExportedSetting,
} from "../src/settingsJson";
import { RPCTestSection, SUBSYSTEM_IDENTIFIER } from "../src/App";
import {
  Notification,
  Request,
  Response,
  Setting,
  SettingNotificationKind,
} from "../src/proto/zmk/custom_settings/custom_settings";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  call_rpc: jest.fn(),
}));

const baseSetting: Setting = {
  customSubsystemIndex: 1,
  key: "int_value",
  meta: undefined,
  hasUnsavedValue: false,
  value: { int32Value: 42 },
  source: 0,
};

describe("RPCTestSection Component", () => {
  beforeEach(() => {
    jest.clearAllMocks();
    (call_rpc as jest.Mock).mockResolvedValue(emptyStatusResponse());
  });

  describe("With Subsystem", () => {
    it("should render RPC controls when subsystem is found", async () => {
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
      expect(
        screen.getByRole("heading", { name: "Filter" })
      ).toBeInTheDocument();
      expect(screen.getByLabelText(/Filter Subsystem/i)).toBeInTheDocument();
      expect(screen.getByLabelText(/Filter Key/i)).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: "Device Settings" })
      ).toBeInTheDocument();
      expect(
        screen.getByText(/Settings load automatically/i)
      ).toBeInTheDocument();
      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(1));
      expect(screen.getByLabelText(/Settings JSON/i)).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Export JSON" })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Import JSON" })
      ).toBeInTheDocument();
      expect(
        screen.queryByRole("heading", { name: "Update Value" })
      ).not.toBeInTheDocument();
    });

    it("should show value editor under clicked scalar setting", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, baseSetting);

      const settingButton = await screen.findByRole(
        "button",
        { name: "test/int_value" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);

      expect(
        screen.getByRole("heading", { name: "Update Value" })
      ).toBeInTheDocument();
      expect(screen.getByText("int32")).toBeInTheDocument();
      expect(screen.queryByText("Array Index")).not.toBeInTheDocument();
      expect(screen.queryByText("Array Size")).not.toBeInTheDocument();
      const input = screen.getByLabelText(/Value/i) as HTMLInputElement;
      expect(input.value).toBe("42");
      expect(
        screen.queryByRole("button", { name: "Push Back" })
      ).not.toBeInTheDocument();
      expect(
        screen.queryByRole("button", { name: "Pop Back" })
      ).not.toBeInTheDocument();

      await userEvent.click(settingButton);
      expect(
        screen.queryByRole("heading", { name: "Update Value" })
      ).not.toBeInTheDocument();
    });

    it("should show array commands for clicked array setting", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        key: "array_value",
        value: {
          arrayValue: {
            index: 0,
            size: 1,
            value: { int32Value: 7 },
          },
        },
      });

      const settingButton = await screen.findByRole(
        "button",
        { name: "test/array_value[0]" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);

      expect(screen.getByText("Array Index")).toBeInTheDocument();
      expect(screen.getByText("Array Size")).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Push Back" })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Pop Back" })
      ).toBeInTheDocument();
    });

    it("should write the selected split source", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(mockZMKApp.onNotification).toHaveBeenCalled());
      emitListItem(mockZMKApp, {
        ...baseSetting,
        source: 2,
      });

      const settingButton = await screen.findByRole(
        "button",
        { name: "test/int_value" },
        { timeout: 2000 }
      );
      await userEvent.click(settingButton);
      await userEvent.click(screen.getByRole("button", { name: "Write" }));

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(2));
      const request = lastCustomSettingsRequest();

      expect(request.writeSetting?.setting).toMatchObject({
        customSubsystemIndex: 1,
        key: "int_value",
        source: 2,
      });
    });

    it("should apply scope actions to all listed split sources", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER, "test"],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(1));
      await screen.findByText("Listed 0 settings");
      await userEvent.click(screen.getByRole("button", { name: "Save" }));

      await waitFor(() => expect(call_rpc).toHaveBeenCalledTimes(2));
      const request = lastCustomSettingsRequest();

      expect(request.saveSettings?.scope?.source).toBe(0xffffffff);
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

function emptyStatusResponse() {
  return {
    custom: {
      call: {
        payload: Response.encode(
          Response.create({ status: { affectedCount: 0, message: "OK" } })
        ).finish(),
      },
    },
  };
}

function emitListItem(
  mockZMKApp: ReturnType<typeof createConnectedMockZMKApp>,
  setting: Setting
) {
  const subscription = (mockZMKApp.onNotification as jest.Mock).mock
    .calls[0][0];
  subscription.callback({
    subsystemIndex: 0,
    payload: Notification.encode(
      Notification.create({
        setting: {
          kind: SettingNotificationKind.SETTING_NOTIFICATION_KIND_LIST_ITEM,
          setting,
        },
      })
    ).finish(),
  });
}

function lastCustomSettingsRequest(): Request {
  const calls = (call_rpc as jest.Mock).mock.calls;
  const request = calls[calls.length - 1][1];
  return Request.decode(request.custom.call.payload);
}

describe("settings JSON conversion", () => {
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
    });
  });

  it("exports JSON grouped by customSubsystems", () => {
    const json = settingsExportToJson([baseSetting], () => "test");
    const parsed = JSON.parse(json);

    expect(parsed.customSubsystems).toBeDefined();
    expect(parsed.settings).toBeUndefined();
    expect(parsed.customSubsystems["test"]).toEqual([
      { key: "int_value", type: "int32", value: 42 },
    ]);
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

  it("parses legacy settings array format", () => {
    const legacy = JSON.stringify({
      format: "zmk-custom-settings",
      version: 1,
      exportedAt: "2024-01-01T00:00:00.000Z",
      settings: [
        {
          customSubsystemId: "test",
          key: "int_value",
          type: "int32",
          value: 42,
        },
      ],
    });
    const [setting] = parseSettingsExportJson(legacy);

    expect(setting).toMatchObject({
      customSubsystemId: "test",
      key: "int_value",
      type: "int32",
      value: 42,
    });
  });
});
