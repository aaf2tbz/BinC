# step: p->x += p->vx*dt etc. Particle = {x,y,z, vx,vy,vz}, dt = 2.0
kernel step
grid 2
buf 0 0.0 0.0 0.0 1.0 2.0 3.0  10.0 10.0 10.0 0.5 0.5 0.5
buf 1 2.0
expect 0 2.0 4.0 6.0 1.0 2.0 3.0  11.0 11.0 11.0 0.5 0.5 0.5
