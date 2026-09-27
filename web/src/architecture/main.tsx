import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import ArchitecturePage from "./ArchitecturePage";
import "./architecture.css";

// Separate entry: reading the guide never mounts the device connection provider.
createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <ArchitecturePage />
  </StrictMode>
);
