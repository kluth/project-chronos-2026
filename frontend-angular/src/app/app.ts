import { Component, OnInit } from '@angular/core';
import { RouterOutlet } from '@angular/router';
import { DashboardComponent } from './dashboard.component';
import { AuthService } from './auth.service';

@Component({
  selector: 'app-root',
  standalone: true,
  imports: [RouterOutlet, DashboardComponent],
  templateUrl: './app.html',
  styleUrl: './app.css'
})
export class App implements OnInit {
  title = 'Project Chronos';

  constructor(private authService: AuthService) {}

  ngOnInit() {
    console.log('Project Chronos UI Initialized.');
    // We optionally invoke FedCM auth on startup
    // this.authService.authenticateWithFedCM();
  }
}
