import { Route, Routes } from "react-router-dom";
import { HomePage, InstancePage } from "./HomePage";
import { HelpPage } from "../features/help/HelpPage";

/** 门户路由：`/help` 为静态手册；`/i/:iid` 为 Feature 实例页。 */
export function App() {
  return (
    <Routes>
      <Route path="/" element={<HomePage />} />
      <Route path="/help" element={<HelpPage />} />
      <Route path="/i/:iid" element={<InstancePage />} />
      <Route path="*" element={<HomePage />} />
    </Routes>
  );
}
