import { Component, ElementRef, HostListener, inject, signal } from '@angular/core';
import { TranslatePipe } from '@ngx-translate/core';
import { Lang, LANG_NAMES, LanguageService } from '../../services/language.service';

/**
 * SDD-080. Language picker for the nav bar.
 *
 * A menu rather than a row of buttons: this frontend ships five languages from
 * the start, and five buttons do not fit a nav bar that already carries eight
 * links. The trigger shows the active language's code so the current state is
 * readable without opening anything.
 *
 * Languages come from LanguageService, so adding one is a single edit there.
 */
@Component({
  selector: 'app-language-switcher',
  standalone: true,
  imports: [TranslatePipe],
  template: `
    <div class="lang-switch">
      <button type="button" class="lang-trigger"
              [attr.aria-label]="'common.language' | translate"
              [attr.aria-expanded]="open()" aria-haspopup="menu"
              (click)="toggle($event)">
        <i class="fa-solid fa-language" aria-hidden="true"></i>
        <span class="code">{{ lang.current().toUpperCase() }}</span>
      </button>

      @if (open()) {
        <div class="lang-menu" role="menu">
          @for (l of lang.available; track l) {
            <button type="button" class="lang-item" role="menuitemradio"
                    [class.active]="lang.current() === l"
                    [attr.aria-checked]="lang.current() === l"
                    (click)="pick(l)">
              <span class="check" aria-hidden="true">{{ lang.current() === l ? '&#10003;' : '' }}</span>
              {{ names[l] }}
            </button>
          }
        </div>
      }
    </div>
  `,
  styles: [`
    .lang-switch { position: relative; display: inline-flex; }
    .lang-trigger {
      display: inline-flex; align-items: center; gap: 6px;
      background: transparent; border: 1px solid #2f2f2f;
      color: #9e9e9e; padding: 0.3rem 0.6rem; border-radius: 4px;
      font-size: 0.8rem; font-weight: 600; cursor: pointer;
      font-family: inherit; transition: all 0.2s;
    }
    .lang-trigger:hover { border-color: #4a9eff; color: #fff; }
    .lang-trigger[aria-expanded="true"] {
      border-color: #4a9eff; color: #fff; background: rgba(74,158,255,0.12);
    }

    .lang-menu {
      position: absolute; top: calc(100% + 6px); right: 0; z-index: 60;
      min-width: 150px; padding: 4px;
      background: #1c1c1c; border: 1px solid #2f2f2f;
      border-radius: 6px; box-shadow: 0 8px 24px rgba(0,0,0,0.5);
      display: flex; flex-direction: column;
    }
    .lang-item {
      display: flex; align-items: center; gap: 8px;
      background: transparent; border: 0; color: #d0d0d0;
      padding: 0.45rem 0.6rem; border-radius: 4px;
      font-size: 0.85rem; cursor: pointer; text-align: left;
      font-family: inherit; white-space: nowrap;
    }
    .lang-item:hover { background: rgba(74,158,255,0.14); color: #fff; }
    .lang-item.active { color: #fff; font-weight: 600; }
    .check { width: 12px; flex: none; color: #4a9eff; }
  `]
})
export class LanguageSwitcherComponent {
  lang = inject(LanguageService);
  private host = inject(ElementRef<HTMLElement>);

  readonly names = LANG_NAMES;
  readonly open = signal(false);

  toggle(event: Event): void {
    event.stopPropagation();
    this.open.update((v) => !v);
  }

  pick(l: Lang): void {
    this.lang.use(l);
    this.open.set(false);
  }

  // A menu that only closes on its own trigger strands itself open over the nav.
  @HostListener('document:click', ['$event'])
  onDocumentClick(event: MouseEvent): void {
    if (!this.open()) return;
    if (!this.host.nativeElement.contains(event.target as Node)) this.open.set(false);
  }

  @HostListener('document:keydown.escape')
  onEscape(): void {
    this.open.set(false);
  }
}
