import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import "./index.css";
import MemoryGuide from "./MemoryGuide";

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <MemoryGuide />
  </StrictMode>
);
