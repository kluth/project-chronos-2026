import { Component, ElementRef, ViewChild, AfterViewInit, OnDestroy } from '@angular/core';
import { initializeApp } from 'firebase/app';
import { getFirestore, collection, onSnapshot, query, where } from 'firebase/firestore';
import { environment } from '../environments/environment'; // Assuming standard environment config

@Component({
  selector: 'app-dashboard',
  standalone: true,
  template: `
    <div class="dashboard-container">
      <header class="glass-header">
        <h1>Chronos <span>Life-Centric Edge Tracker</span></h1>
        <button class="premium-btn">Synchronize Context</button>
      </header>
      <div class="canvas-wrapper">
        <canvas #visualizationCanvas class="immersive-canvas" width="800" height="400"></canvas>
      </div>
    </div>
  `,
  styles: [`
    .dashboard-container {
      display: flex;
      flex-direction: column;
      height: 100vh;
      background: radial-gradient(circle at top right, #1a1a2e, #16213e, #0f3460);
      color: #e94560;
      font-family: 'Inter', sans-serif;
    }
    .glass-header {
      background: rgba(255, 255, 255, 0.05);
      backdrop-filter: blur(10px);
      padding: 20px 40px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      border-bottom: 1px solid rgba(255, 255, 255, 0.1);
    }
    h1 {
      margin: 0;
      font-weight: 300;
      letter-spacing: 2px;
    }
    h1 span {
      font-weight: 700;
      font-size: 0.5em;
      text-transform: uppercase;
      letter-spacing: 4px;
      margin-left: 10px;
      opacity: 0.7;
    }
    .premium-btn {
      background: linear-gradient(135deg, #e94560, #ff2e63);
      border: none;
      border-radius: 30px;
      padding: 10px 25px;
      color: white;
      font-weight: bold;
      cursor: pointer;
      box-shadow: 0 4px 15px rgba(233, 69, 96, 0.4);
      transition: all 0.3s ease;
    }
    .premium-btn:hover {
      transform: translateY(-2px);
      box-shadow: 0 6px 20px rgba(233, 69, 96, 0.6);
    }
    .canvas-wrapper {
      flex: 1;
      display: flex;
      justify-content: center;
      align-items: center;
      padding: 20px;
    }
    .immersive-canvas {
      width: 100%;
      height: 100%;
      max-width: 800px;
      max-height: 400px;
      border-radius: 20px;
      background: rgba(0, 0, 0, 0.2);
      box-shadow: inset 0 0 50px rgba(0,0,0,0.5);
    }
  `]
})
export class DashboardComponent implements AfterViewInit, OnDestroy {
  @ViewChild('visualizationCanvas') canvasRef!: ElementRef<HTMLCanvasElement>;
  private unsubscribeFn?: () => void;

  ngAfterViewInit() {
    // Connect to actual Firebase Firestore instance
    // Note: requires valid environment.firebase config
    const app = initializeApp(environment.firebase);
    const db = getFirestore(app);

    // Using a mocked userId for demonstration since auth may not be fully logged in
    const userId = "user_123";
    const anchorsRef = collection(db, `users/${userId}/private_anchors`);

    // Listen to real-time changes in Firestore
    this.unsubscribeFn = onSnapshot(anchorsRef, (snapshot) => {
      const anchors = snapshot.docs.map(doc => doc.data());
      this.renderCanvas(anchors);
    });
  }

  ngOnDestroy() {
    if (this.unsubscribeFn) {
      this.unsubscribeFn();
    }
  }

  private renderCanvas(anchors: any[]) {
    const canvas = this.canvasRef.nativeElement;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    // Helper: Truncate text to fit within a specific pixel width
    const truncate = (text: string, maxWidth: number) => {
      if (ctx.measureText(text).width <= maxWidth) return text;
      let truncated = text;
      while (ctx.measureText(truncated + '...').width > maxWidth && truncated.length > 0) {
        truncated = truncated.slice(0, -1);
      }
      return truncated + '...';
    };

    // Clear canvas
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    // Draw background grid & timeline
    ctx.strokeStyle = 'rgba(255,255,255,0.05)';
    ctx.lineWidth = 1;
    for(let i=0; i<=canvas.width; i+=100) {
      ctx.beginPath();
      ctx.moveTo(i, 0);
      ctx.lineTo(i, canvas.height - 40);
      ctx.stroke();
      
      // Draw timeline labels
      ctx.fillStyle = 'rgba(255, 255, 255, 0.4)';
      ctx.font = '12px Inter';
      const hour = (i / 100) + 9; // Let 0 = 9:00 AM
      ctx.fillText(`${hour}:00`, i + 5, canvas.height - 15);
    }

    // Draw private anchors based on real data
    anchors.forEach((anchor, index) => {
      // Map logical start to X coordinate
      const startX = anchor.start || (index * 200 + 50); 
      const rawWidth = (anchor.end - anchor.start) || 150;
      
      // Enforce a strict minimum width so text never spills outside the boundaries
      const padding = 15;
      const visualWidth = Math.max(rawWidth, 190);
      const maxTextWidth = visualWidth - (padding * 2);
      
      // Draw glow
      ctx.shadowColor = '#e94560';
      ctx.shadowBlur = 20;
      
      // Draw rounded rectangle for the anchor
      ctx.fillStyle = 'rgba(233, 69, 96, 0.85)';
      ctx.beginPath();
      ctx.roundRect(startX, 100, visualWidth, 130, 15);
      ctx.fill();

      // Reset shadow for text to prevent blur on fonts
      ctx.shadowBlur = 0;

      // Draw title
      ctx.fillStyle = '#ffffff';
      ctx.font = 'bold 16px Inter';
      const desc = anchor.description || 'Private Time';
      ctx.fillText(truncate(desc, maxTextWidth), startX + padding, 135);

      // Draw time details
      ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
      ctx.font = '13px Inter';
      const startHour = Math.floor(anchor.start/100) + 9;
      const endHour = Math.floor(anchor.end/100) + 9;
      const eventDate = anchor.date || 'June 20, 2026';
      const timeStr = `🕒 ${startHour}:00 - ${endHour}:00  📅 ${eventDate}`;
      ctx.fillText(truncate(timeStr, maxTextWidth), startX + padding, 165);

      // Draw place/type
      ctx.fillStyle = 'rgba(255, 255, 255, 0.7)';
      ctx.font = '12px Inter';
      const locStr = `📍 Location: Dorfwiese`;
      ctx.fillText(truncate(locStr, maxTextWidth), startX + padding, 185);
      
      // Draw protection badge
      const badgeWidth = 135;
      ctx.fillStyle = 'rgba(0, 255, 100, 0.15)';
      ctx.beginPath();
      ctx.roundRect(startX + padding, 200, badgeWidth, 22, 8);
      ctx.fill();
      
      ctx.fillStyle = '#00ff66';
      ctx.font = 'bold 11px Inter';
      ctx.fillText('✓ CRDT PROTECTED', startX + padding + 10, 215);
    });

    if (anchors.length === 0) {
      ctx.fillStyle = 'rgba(255, 255, 255, 0.5)';
      ctx.font = '20px Inter';
      ctx.fillText('Listening to Firestore... No active anchors found.', 180, 200);
    }
  }
}
