export type EquipmentCategory = 'machine' | 'accessory';
export type SupplyState = 'fresh' | 'due_soon' | 'overdue' | 'untracked';

export interface EquipmentType {
  id: number;
  type_key: string;
  label: string;
  category: EquipmentCategory;
  default_replace_after_days: number | null;
  is_system: boolean;
  custom: boolean;
  active: boolean;
}

export interface SupplyInfo {
  state: SupplyState;
  days_left: number;
  wear_fraction: number;
  replace_by: number;
}

export interface EquipmentItem {
  id: number;
  profile_id: number;
  client_uuid: string;
  type_key: string;
  slot: string;
  category: EquipmentCategory;
  brand: string;
  model: string;
  variant: string;
  started_using_at: string;
  notes: string;
  active: boolean;
  replace_after_days: number | null;
  created_at: string;
  updated_at: string;
  supply: SupplyInfo;
}

export interface EquipmentProfile {
  id: number;
  client_uuid: string;
  name: string;
  active: boolean;
  created_at: string;
  updated_at: string;
  items: EquipmentItem[];
}

/** Payload for POST /api/equipment and PUT /api/equipment/{id}. */
export interface EquipmentItemPayload {
  profile_id: number;
  client_uuid?: string;
  type_key: string;
  brand: string;
  model: string;
  variant?: string;
  started_using_at: string;
  notes?: string;
  active?: boolean;
  replace_after_days: number | null;
}
