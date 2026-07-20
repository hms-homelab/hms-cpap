import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { forkJoin } from 'rxjs';
import { CpapApiService } from '../../services/cpap-api.service';
import {
  EquipmentItem,
  EquipmentItemPayload,
  EquipmentProfile,
  EquipmentType,
  SupplyState
} from '../../models/equipment.model';

/** Inline edit buffer for one item. Strings so blank means "unset". */
interface ItemDraft {
  brand: string;
  model: string;
  since: string;        // yyyy-mm-dd for <input type="date">
  replaceEvery: string | number | null; // blank = use the type default
}

@Component({
  selector: 'app-equipment',
  standalone: true,
  imports: [CommonModule, FormsModule],
  templateUrl: './equipment.component.html',
  styleUrls: ['./equipment.component.css']
})
export class EquipmentComponent implements OnInit {
  loading = true;
  error = '';
  busy = false;

  profiles: EquipmentProfile[] = [];
  types: EquipmentType[] = [];
  selectedId = 0;

  // Inline editors
  editingItemId = 0;
  draft: ItemDraft = { brand: '', model: '', since: '', replaceEvery: '' };
  renaming = false;
  renameValue = '';
  creatingProfile = false;
  newProfileName = '';
  addTypeKey = '';

  constructor(private api: CpapApiService) {}

  ngOnInit() {
    this.load();
  }

  load(selectId?: number) {
    this.loading = true;
    this.error = '';
    forkJoin({
      types: this.api.getEquipmentTypes(),
      profiles: this.api.getEquipmentProfiles()
    }).subscribe({
      next: r => {
        this.types = r.types.types || [];
        this.profiles = r.profiles.profiles || [];
        const wanted = selectId ?? this.selectedId;
        this.selectedId = this.profiles.some(p => p.id === wanted)
          ? wanted
          : (this.profiles[0]?.id ?? 0);
        this.cancelEdit();
        this.loading = false;
      },
      error: () => {
        this.error = 'Failed to load equipment.';
        this.loading = false;
      }
    });
  }

  // ── Profiles ───────────────────────────────────────────────────────────────

  get selected(): EquipmentProfile | null {
    return this.profiles.find(p => p.id === this.selectedId) ?? null;
  }

  select(p: EquipmentProfile) {
    if (this.selectedId === p.id) return;
    this.selectedId = p.id;
    this.cancelEdit();
    this.renaming = false;
    this.addTypeKey = '';
  }

  /** A chip is badged when that profile has anything due soon or overdue. */
  hasAlert(p: EquipmentProfile): boolean {
    return p.items.some(i => i.supply.state === 'due_soon' || i.supply.state === 'overdue');
  }

  startCreateProfile() {
    this.creatingProfile = true;
    this.newProfileName = '';
  }

  cancelCreateProfile() {
    this.creatingProfile = false;
    this.newProfileName = '';
  }

  createProfile() {
    const name = this.newProfileName.trim();
    if (!name || this.busy) return;
    this.busy = true;
    this.api.createEquipmentProfile(name).subscribe({
      next: p => {
        this.busy = false;
        this.creatingProfile = false;
        this.newProfileName = '';
        this.load(p?.id);
      },
      error: () => {
        this.busy = false;
        this.error = 'Failed to create the profile.';
      }
    });
  }

  startRename() {
    if (!this.selected) return;
    this.renaming = true;
    this.renameValue = this.selected.name;
  }

  cancelRename() {
    this.renaming = false;
  }

  saveRename() {
    const p = this.selected;
    const name = this.renameValue.trim();
    if (!p || !name || this.busy) return;
    this.busy = true;
    this.api.renameEquipmentProfile(p.id, name).subscribe({
      next: () => {
        this.busy = false;
        this.renaming = false;
        this.load(p.id);
      },
      error: () => {
        this.busy = false;
        this.error = 'Failed to rename the profile.';
      }
    });
  }

  deleteProfile() {
    const p = this.selected;
    if (!p || this.busy) return;
    if (!confirm(`Remove the profile "${p.name}" and everything in it?`)) return;
    this.busy = true;
    this.api.deleteEquipmentProfile(p.id).subscribe({
      next: () => {
        this.busy = false;
        this.selectedId = 0;
        this.load();
      },
      error: () => {
        this.busy = false;
        this.error = 'Failed to remove the profile.';
      }
    });
  }

  // ── Items ──────────────────────────────────────────────────────────────────

  get machine(): EquipmentItem | null {
    return this.selected?.items.find(i => i.category === 'machine') ?? null;
  }

  get accessories(): EquipmentItem[] {
    return (this.selected?.items ?? []).filter(i => i.category !== 'machine');
  }

  /** Catalog options for "add"; the machine drops out once the profile has one. */
  get addableTypes(): EquipmentType[] {
    const hasMachine = !!this.machine;
    return this.types.filter(t => t.active && !(hasMachine && t.category === 'machine'));
  }

  typeLabel(key: string): string {
    return this.types.find(t => t.type_key === key)?.label ?? key;
  }

  /** Placeholder for the replace-every field when the item has no override. */
  defaultDays(item: EquipmentItem): number | null {
    return this.types.find(t => t.type_key === item.type_key)?.default_replace_after_days ?? null;
  }

  addItem() {
    const p = this.selected;
    const key = this.addTypeKey;
    if (!p || !key || this.busy) return;
    this.busy = true;
    const payload: EquipmentItemPayload = {
      profile_id: p.id,
      type_key: key,
      brand: '',
      model: '',
      started_using_at: this.toIso(this.today()),
      replace_after_days: null
    };
    this.api.createEquipmentItem(payload).subscribe({
      next: () => {
        this.busy = false;
        this.addTypeKey = '';
        this.load(p.id);
      },
      error: err => {
        this.busy = false;
        this.error = err?.error?.error || 'Failed to add the item.';
      }
    });
  }

  isEditing(item: EquipmentItem): boolean {
    return this.editingItemId === item.id;
  }

  startEdit(item: EquipmentItem) {
    this.editingItemId = item.id;
    this.draft = {
      brand: item.brand || '',
      model: item.model || '',
      since: this.toDateInput(item.started_using_at),
      replaceEvery: item.replace_after_days == null ? '' : String(item.replace_after_days)
    };
  }

  cancelEdit() {
    this.editingItemId = 0;
    this.draft = { brand: '', model: '', since: '', replaceEvery: '' };
  }

  saveEdit(item: EquipmentItem) {
    if (this.busy) return;
    // type="number" hands back a number (or null), type="text" a string — accept both.
    const raw = this.draft.replaceEvery;
    const days = raw === null || raw === undefined ? '' : String(raw).trim();
    const parsed = days === '' ? null : Number(days);
    if (parsed !== null && (!Number.isFinite(parsed) || parsed < 0)) {
      this.error = 'Replace every must be a number of days.';
      return;
    }
    this.busy = true;
    this.error = '';
    // The API replaces the whole row, so unedited fields are echoed back verbatim.
    const payload: EquipmentItemPayload = {
      profile_id: item.profile_id,
      client_uuid: item.client_uuid,
      type_key: item.type_key,
      brand: this.draft.brand.trim(),
      model: this.draft.model.trim(),
      variant: item.variant,
      started_using_at: this.toIso(this.draft.since),
      notes: item.notes,
      active: item.active,
      replace_after_days: parsed
    };
    this.api.updateEquipmentItem(item.id, payload).subscribe({
      next: () => {
        this.busy = false;
        this.cancelEdit();
        this.load(this.selectedId);
      },
      error: err => {
        this.busy = false;
        this.error = err?.error?.error || 'Failed to save the item.';
      }
    });
  }

  removeItem(item: EquipmentItem) {
    if (this.busy) return;
    if (!confirm(`Remove this ${this.typeLabel(item.type_key).toLowerCase()}?`)) return;
    this.busy = true;
    this.api.deleteEquipmentItem(item.id).subscribe({
      next: () => {
        this.busy = false;
        this.load(this.selectedId);
      },
      error: () => {
        this.busy = false;
        this.error = 'Failed to remove the item.';
      }
    });
  }

  // ── Wear presentation ──────────────────────────────────────────────────────

  wearPercent(item: EquipmentItem): number {
    const f = item.supply?.wear_fraction ?? 0;
    if (!Number.isFinite(f) || f <= 0) return 0;
    return Math.min(100, Math.round(f * 100));
  }

  stateLabel(item: EquipmentItem): string {
    const s = item.supply?.state as SupplyState;
    const d = item.supply?.days_left ?? 0;
    if (s === 'untracked') return 'Not tracked';
    if (s === 'overdue') return `${Math.abs(d)} ${this.plural(Math.abs(d))} overdue`;
    return `${d} ${this.plural(d)} left`;
  }

  replaceByLabel(item: EquipmentItem): string {
    if (item.supply?.state === 'untracked' || !item.supply?.replace_by) return '';
    return new Date(item.supply.replace_by * 1000).toISOString().slice(0, 10);
  }

  private plural(n: number): string {
    return n === 1 ? 'day' : 'days';
  }

  // ── Date helpers (API is ISO-8601 UTC; the input wants yyyy-mm-dd) ─────────

  private today(): string {
    return new Date().toISOString().slice(0, 10);
  }

  toDateInput(iso: string): string {
    return iso ? iso.slice(0, 10) : '';
  }

  private toIso(date: string): string {
    return date ? `${date}T00:00:00Z` : '';
  }
}
