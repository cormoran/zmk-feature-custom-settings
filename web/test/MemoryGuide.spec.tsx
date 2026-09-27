import { fireEvent, render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import MemoryGuide from "../src/MemoryGuide";

describe("Memory guide", () => {
  it("is readable without a device and links each section", () => {
    render(<MemoryGuide />);
    expect(screen.getByRole("heading", { level: 1 })).toHaveTextContent(
      "Where do your settings live?"
    );
    for (const link of screen.getByRole("navigation").querySelectorAll("a")) {
      expect(document.querySelector(link.hash)).toBeInTheDocument();
    }
    expect(
      screen.getByRole("link", { name: /Device settings console/ })
    ).toHaveAttribute("href", "./");
  });

  it("distinguishes empty bytes from string terminators without releasing backing RAM", async () => {
    const user = userEvent.setup();
    render(<MemoryGuide />);
    await user.click(
      screen.getByRole("button", { name: "Empty all BYTES values" })
    );
    expect(screen.getByRole("status")).toHaveTextContent(
      "0 / 256 bytes occupied"
    );
    await user.click(screen.getByRole("checkbox"));
    expect(screen.getByRole("status")).toHaveTextContent(
      "3 / 256 bytes occupied"
    );
    expect(
      screen.getByText("Reserved backing RAM: always 256 bytes")
    ).toBeInTheDocument();
  });

  it("shows exact capacity, rejects overflow, and recovers when the proposal fits", () => {
    render(<MemoryGuide />);
    const sliders = screen.getAllByRole("slider");
    fireEvent.change(sliders[0], { target: { value: "128" } });
    fireEvent.change(sliders[1], { target: { value: "128" } });
    expect(screen.getByRole("status")).toHaveTextContent(
      "256 / 256 bytes occupied · 0 bytes available"
    );
    fireEvent.change(sliders[2], { target: { value: "1" } });
    expect(screen.getByRole("status")).toHaveTextContent(
      "257 / 256 bytes requested · write fails with -ENOSPC; the previous value stays unchanged."
    );
    fireEvent.change(sliders[0], { target: { value: "0" } });
    expect(screen.getByRole("status")).toHaveTextContent(
      "129 / 256 bytes occupied"
    );
  });
});
