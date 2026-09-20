import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import path from "node:path";

export default defineConfig({
  plugins: [react()],
  base: "/",
  build: {
    outDir: path.resolve(__dirname, "../rdbg/static_ui"),
    emptyOutDir: true,
  },
  server: {
    port: 5173,
    proxy: {
      "/api": "http://127.0.0.1:8080",
      "/static": "http://127.0.0.1:8080",
    },
  },
});
