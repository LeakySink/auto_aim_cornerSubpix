import { Route, Routes } from "react-router-dom";
import { HomePage, InstancePage } from "./HomePage";

export function App() {
  return (
    <Routes>
      <Route path="/" element={<HomePage />} />
      <Route path="/i/:iid" element={<InstancePage />} />
      <Route path="*" element={<HomePage />} />
    </Routes>
  );
}
