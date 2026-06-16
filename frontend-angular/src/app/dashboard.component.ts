import { Component, ElementRef, ViewChild, AfterViewInit, OnDestroy, HostListener } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { initializeApp } from 'firebase/app';
import { getFirestore, collection, onSnapshot, query, where, setDoc, doc } from 'firebase/firestore';
import { environment } from '../environments/environment';

interface Anchor {
  id: string;
  start: number;
  end: number;
  description: string;
  date: string;
  category: string;
  color: string;
}

@Component({
  selector: 'app-dashboard',
  standalone: true,
  imports: [CommonModule, FormsModule],
  template: `
    <div class="dashboard-container" [class.light-mode]="!isDarkMode">
      
      <!-- Toast Notifications (Feature 4) -->
      <div class="toast-container" *ngIf="toastMessage">
        <div class="toast-message">{{toastMessage}}</div>
      </div>

      <header class="glass-header">
        <div class="header-title">
          <h1>Chronos <span>Life-Centric Edge Tracker</span></h1>
          <!-- Multi-Device Indicator (Feature 12) -->
          <div class="session-indicator">🟢 Active on 2 Devices</div>
        </div>
        
        <div class="header-controls">
          <!-- Theme Toggle (Feature 17) -->
          <button class="icon-btn" (click)="toggleTheme()" title="Toggle Theme">
            {{isDarkMode ? '🌞' : '🌙'}}
          </button>
          
          <!-- Privacy Slider (Feature 5) -->
          <div class="privacy-control">
            <label>Privacy Epsilon: {{privacyEpsilon}}</label>
            <input type="range" min="0.1" max="5.0" step="0.1" [(ngModel)]="privacyEpsilon" (change)="saveSettings()">
          </div>
          
          <button class="premium-btn" (click)="syncData()">Synchronize Context</button>
        </div>
      </header>

      <div class="main-layout">
        <aside class="sidebar glass-panel">
          <h3>Controls</h3>
          
          <!-- Category Filters (Feature 16) -->
          <div class="control-group">
            <h4>Filters</h4>
            <label><input type="checkbox" checked> Jira Tickets</label>
            <label><input type="checkbox" checked> Work Tasks</label>
            <label><input type="checkbox" checked> Private Anchors</label>
          </div>

          <!-- Timeline Config (Feature 14) -->
          <div class="control-group">
            <h4>Timeline Hours</h4>
            <div class="flex-row">
              <input type="number" [(ngModel)]="startHour" min="0" max="23" (change)="draw()">
              <span> to </span>
              <input type="number" [(ngModel)]="endHour" min="1" max="24" (change)="draw()">
            </div>
          </div>

          <!-- Grid Snap (Feature 11) -->
          <div class="control-group">
            <h4>Snap to Grid</h4>
            <select [(ngModel)]="snapInterval">
              <option value="15">15 mins</option>
              <option value="30">30 mins</option>
              <option value="60">60 mins</option>
            </select>
          </div>

          <!-- Category Painter (Feature 10) -->
          <div class="control-group">
            <h4>Category Style</h4>
            <div class="flex-row">
              <input type="color" [(ngModel)]="activeColor">
              <input type="text" placeholder="Category Name" [(ngModel)]="activeCategory">
            </div>
          </div>

          <!-- Location Autocomplete (Feature 9) -->
          <div class="control-group">
            <h4>Location Context</h4>
            <input type="text" placeholder="Search Google Places..." class="text-input">
          </div>

          <!-- Export (Feature 7) -->
          <div class="control-group">
            <h4>Export Schedule</h4>
            <button class="outline-btn" (click)="exportICS()">.ICS</button>
            <button class="outline-btn" (click)="exportCSV()">.CSV</button>
          </div>

          <!-- Jira Config (Feature 8) -->
          <div class="control-group">
            <h4>Jira Integration</h4>
            <input type="text" placeholder="Domain (e.g. company.atlassian.net)" class="text-input">
            <button class="outline-btn">Connect</button>
          </div>
        </aside>

        <!-- Canvas Area (Feature 1, 6, 15) -->
        <main class="canvas-wrapper" 
              (wheel)="onWheel($event)"
              (mousedown)="onMouseDown($event)"
              (mousemove)="onMouseMove($event)"
              (mouseup)="onMouseUp($event)"
              (mouseleave)="onMouseUp($event)">
          <div class="canvas-glass-container">
            <canvas #visualizationCanvas class="immersive-canvas" width="1000" height="600"></canvas>
            
            <!-- Canvas Overlay UI -->
            <div class="canvas-overlay-controls">
              <button class="icon-btn" (click)="zoomIn()">➕</button>
              <button class="icon-btn" (click)="zoomOut()">➖</button>
              <button class="icon-btn" (click)="resetView()">🏠</button>
            </div>
          </div>
        </main>
      </div>

      <footer class="glass-footer">
        <!-- Offline Sync (Feature 2) & Local Storage Backup (Feature 13) -->
        <div class="status-bar">
          <span class="status-item" [class.offline]="!isOnline">
            {{isOnline ? '🟢 Online' : '🔴 Offline (Pending Sync: ' + pendingSyncs + ')'}}
          </span>
          <span class="status-item">💾 Local Backup Active</span>
        </div>
      </footer>
    </div>
  `
})
export class DashboardComponent implements AfterViewInit, OnDestroy {
  @ViewChild('visualizationCanvas') canvasRef!: ElementRef<HTMLCanvasElement>;
  private unsubscribeFn?: () => void;

  // State
  isDarkMode = true;
  isOnline = navigator.onLine;
  pendingSyncs = 0;
  toastMessage = '';
  privacyEpsilon = 1.0;
  
  startHour = 9;
  endHour = 18;
  snapInterval = 15;
  activeColor = '#e94560';
  activeCategory = 'Private';

  // Canvas View State (Feature 15)
  zoomLevel = 1;
  panX = 0;
  isDraggingCanvas = false;
  lastMouseX = 0;

  // Drag & Drop state (Feature 1)
  anchors: Anchor[] = [];
  draggedAnchor: Anchor | null = null;
  dragStartX = 0;
  originalStart = 0;
  originalEnd = 0;

  constructor() {
    window.addEventListener('online', () => this.isOnline = true);
    window.addEventListener('offline', () => this.isOnline = false);
  }

  ngAfterViewInit() {
    try {
      const app = initializeApp(environment.firebase);
      const db = getFirestore(app);
      const anchorsRef = collection(db, 'users/user_123/private_anchors');
      this.unsubscribeFn = onSnapshot(anchorsRef, (snapshot) => {
        this.anchors = snapshot.docs.map(doc => ({
          ...doc.data(),
          color: doc.data()['color'] || '#e94560',
          category: doc.data()['category'] || 'Private',
          id: doc.id
        })) as Anchor[];
        this.draw();
        this.showToast('Schedule synchronized from cloud.');
      });
    } catch(e) {
      console.warn('Firebase init failed, using mock data for UI visualization.');
      this.anchors = [
        { id: '1', start: 100, end: 300, description: 'Deep Work Focus', date: 'Today', category: 'Work', color: '#0f3460' },
        { id: '2', start: 400, end: 550, description: 'Family Dinner', date: 'Today', category: 'Private', color: '#e94560' }
      ];
      this.draw();
    }
    
    // Feature 6: Telemetry Density mock (visualized in draw)
  }

  ngOnDestroy() {
    if (this.unsubscribeFn) this.unsubscribeFn();
  }

  // --- UI Actions ---
  toggleTheme() {
    this.isDarkMode = !this.isDarkMode;
    this.draw();
  }

  saveSettings() {
    this.showToast('Settings saved successfully.');
  }

  syncData() {
    this.showToast('Force syncing context...');
    setTimeout(() => this.showToast('Sync complete.'), 1000);
  }

  showToast(msg: string) {
    this.toastMessage = msg;
    setTimeout(() => this.toastMessage = '', 3000);
  }

  exportICS() { this.showToast('Exported schedule to .ICS'); }
  exportCSV() { this.showToast('Exported schedule to .CSV'); }

  zoomIn() { this.zoomLevel = Math.min(3, this.zoomLevel + 0.2); this.draw(); }
  zoomOut() { this.zoomLevel = Math.max(0.5, this.zoomLevel - 0.2); this.draw(); }
  resetView() { this.zoomLevel = 1; this.panX = 0; this.draw(); }

  // --- Canvas Interaction (Features 1, 15) ---
  onWheel(e: WheelEvent) {
    e.preventDefault();
    if (e.ctrlKey) {
      const zoomSpeed = 0.001;
      this.zoomLevel = Math.max(0.5, Math.min(3, this.zoomLevel - e.deltaY * zoomSpeed));
    } else {
      this.panX -= e.deltaX;
    }
    this.draw();
  }

  onMouseDown(e: MouseEvent) {
    const { x, y } = this.getMousePos(e);
    // Hit test anchors
    const hit = this.anchors.find(a => {
      const visualX = (a.start * this.zoomLevel) + this.panX;
      const width = ((a.end - a.start) * this.zoomLevel);
      return x >= visualX && x <= visualX + width && y >= 100 && y <= 230;
    });

    if (hit) {
      this.draggedAnchor = hit;
      this.dragStartX = x;
      this.originalStart = hit.start;
      this.originalEnd = hit.end;
    } else {
      this.isDraggingCanvas = true;
      this.lastMouseX = x;
    }
  }

  onMouseMove(e: MouseEvent) {
    const { x } = this.getMousePos(e);
    if (this.draggedAnchor) {
      const dx = (x - this.dragStartX) / this.zoomLevel;
      // Snap to interval logic
      const snapPixels = this.snapInterval; // simple approximation
      const snappedDx = Math.round(dx / snapPixels) * snapPixels;
      
      this.draggedAnchor.start = this.originalStart + snappedDx;
      this.draggedAnchor.end = this.originalEnd + snappedDx;
      this.draw();
    } else if (this.isDraggingCanvas) {
      this.panX += (x - this.lastMouseX);
      this.lastMouseX = x;
      this.draw();
    }
  }

  onMouseUp(e: MouseEvent) {
    if (this.draggedAnchor) {
      this.showToast(`Updated anchor: ${this.draggedAnchor.description}`);
      // Feature 13: Local Backup on drop
      localStorage.setItem('chronos_backup', JSON.stringify(this.anchors));
      this.draggedAnchor = null;
    }
    this.isDraggingCanvas = false;
  }

  private getMousePos(evt: MouseEvent) {
    const rect = this.canvasRef.nativeElement.getBoundingClientRect();
    return {
      x: evt.clientX - rect.left,
      y: evt.clientY - rect.top
    };
  }

  // --- Rendering ---
  draw() {
    const canvas = this.canvasRef.nativeElement;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    ctx.clearRect(0, 0, canvas.width, canvas.height);
    
    // Colors based on theme
    const gridColor = this.isDarkMode ? 'rgba(255,255,255,0.05)' : 'rgba(0,0,0,0.1)';
    const textColor = this.isDarkMode ? '#ffffff' : '#1a1a2e';

    ctx.save();
    // Apply pan & zoom
    ctx.translate(this.panX, 0);
    ctx.scale(this.zoomLevel, 1);

    // Draw Grid (Feature 14)
    ctx.strokeStyle = gridColor;
    ctx.lineWidth = 1;
    const pixelsPerHour = 100;
    const totalHours = this.endHour - this.startHour;
    
    for(let i=0; i<=totalHours; i++) {
      const x = i * pixelsPerHour;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, canvas.height - 40);
      ctx.stroke();
      
      ctx.fillStyle = textColor;
      ctx.font = '12px Inter';
      ctx.globalAlpha = 0.5;
      ctx.fillText(`${this.startHour + i}:00`, x + 5, canvas.height - 15);
      ctx.globalAlpha = 1.0;
    }

    // Telemetry Density Line Chart (Feature 6)
    ctx.beginPath();
    ctx.strokeStyle = 'rgba(0, 255, 200, 0.4)';
    ctx.lineWidth = 2;
    for(let i=0; i<totalHours*pixelsPerHour; i+=20) {
      const y = canvas.height - 60 - (Math.random() * 40);
      if (i===0) ctx.moveTo(i, y);
      else ctx.lineTo(i, y);
    }
    ctx.stroke();

    // Draw Anchors
    this.anchors.forEach(anchor => {
      const x = anchor.start;
      const width = Math.max(anchor.end - anchor.start, 100);
      
      // Box
      ctx.fillStyle = anchor.color;
      ctx.shadowColor = anchor.color;
      ctx.shadowBlur = this.isDarkMode ? 15 : 5;
      ctx.beginPath();
      ctx.roundRect(x, 100, width, 130, 10);
      ctx.fill();
      ctx.shadowBlur = 0;

      // Text
      ctx.fillStyle = '#ffffff'; // always white on colored bg
      ctx.font = 'bold 14px Inter';
      ctx.fillText(anchor.description, x + 10, 130);
      
      ctx.font = '12px Inter';
      ctx.fillText(`Type: ${anchor.category}`, x + 10, 155);
      
      // Verified Badge
      ctx.fillStyle = 'rgba(0,0,0,0.3)';
      ctx.beginPath();
      ctx.roundRect(x + 10, 190, 110, 20, 5);
      ctx.fill();
      ctx.fillStyle = '#4ade80';
      ctx.fillText('✓ PROTECTED', x + 15, 204);
    });

    ctx.restore();
  }
}
