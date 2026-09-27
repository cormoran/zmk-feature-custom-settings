import { fireEvent, render, screen, within } from "@testing-library/react";
import ArchitecturePage from "../src/architecture/ArchitecturePage";

// The guide must remain usable without connection providers or a device.
describe("architecture guide", () => {
  it("switches the explained compatibility layout, including the state sizes", () => {
    render(<ArchitecturePage />);
    const figure = screen.getByRole("figure", {
      name: "03 — value の実際の配置",
    });
    expect(within(figure).getByText(/二つの struct 定義/)).toBeInTheDocument();
    expect(
      screen.getByRole("table", { name: /互換 OFF の state/ })
    ).toHaveTextContent("2 B");
    fireEvent.click(screen.getByRole("radio", { name: /ON —/ }));
    expect(within(figure).getByText("76 B")).toBeInTheDocument();
    expect(
      screen.getByRole("table", { name: /互換 ON の state/ })
    ).toHaveTextContent("20 B");
    fireEvent.click(screen.getByRole("radio", { name: /OFF —/ }));
    expect(within(figure).queryByText("76 B")).not.toBeInTheDocument();
  });

  it("explains the ROM to pool transition and can return to the default", () => {
    render(<ArchitecturePage />);
    const figure = screen.getByRole("figure", {
      name: "08 — blob の初期値と編集後",
    });
    expect(within(figure).getByText(/extent=0/)).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "編集後の配置を見る" }));
    expect(within(figure).getByText(/extent=5/)).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "既定値の状態へ戻す" }));
    expect(within(figure).getByText(/extent=0/)).toBeInTheDocument();
  });

  it("has reachable chapter anchors and separate console navigation", () => {
    render(<ArchitecturePage />);
    for (const link of within(screen.getByRole("navigation")).getAllByRole(
      "link"
    )) {
      expect(
        document.getElementById(link.getAttribute("href")!.slice(1))
      ).not.toBeNull();
    }
    expect(
      screen.getByRole("link", { name: /Settings console/ })
    ).toHaveAttribute("href", "./");
  });
});
