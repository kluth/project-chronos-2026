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

    // Clear canvas
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    // Draw background grid
    ctx.strokeStyle = 'rgba(255,255,255,0.1)';
    for(let i=0; i<canvas.width; i+=50) {
      ctx.beginPath();
      ctx.moveTo(i, 0);
      ctx.lineTo(i, canvas.height);
      ctx.stroke();
    }

    // Draw private anchors based on real data
    ctx.fillStyle = 'rgba(233, 69, 96, 0.8)'; // Core theme color for anchors
    anchors.forEach((anchor, index) => {
      // Very basic normalization of timestamps to canvas width (assuming a 24h window)
      // For this implementation, we simulate mapping
      const startX = (index * 150) + 50; 
      const width = 100;
      
      // Draw rounded rectangle for the anchor
      ctx.beginPath();
      ctx.roundRect(startX, 150, width, 100, 10);
      ctx.fill();

      // Draw text
      ctx.fillStyle = '#ffffff';
      ctx.font = '14px Inter';
      ctx.fillText(anchor['description']?.substring(0, 12) + '...', startX + 10, 200);
      ctx.fillStyle = 'rgba(233, 69, 96, 0.8)';
    });

    if (anchors.length === 0) {
      ctx.fillStyle = 'rgba(255, 255, 255, 0.5)';
      ctx.font = '20px Inter';
      ctx.fillText('Listening to Firestore... No active anchors found.', 180, 200);
    }
  }
}
