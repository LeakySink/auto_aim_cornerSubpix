import type { FC } from "react";

export type FeatureMeta = {
  id: string;
  title: string;
  description: string;
  state?: string;
};

export type FeatureModule = {
  id: string;
  title: string;
  description: string;
  route: string;
  Component?: FC;
};

export type FeatureStatus = {
  id: string;
  state: string;
  error?: string;
  config?: Record<string, unknown>;
  feature?: string;
  sender?: string;
  data_port?: number;
};
