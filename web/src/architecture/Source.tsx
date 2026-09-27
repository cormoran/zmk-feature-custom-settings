import type { ReactNode } from "react";
import { sourceRoot } from "./content";

export function Source({
  path,
  children,
}: {
  path: string;
  children?: ReactNode;
}) {
  return (
    <a href={sourceRoot + path} target="_blank" rel="noreferrer">
      {children ?? path}
    </a>
  );
}
