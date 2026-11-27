#pragma once
// Coordinates are in a 1650 x 927 reference canvas (normalized from the
// supplied 1919 x 1079 screenshot). Keep CPU, CUDA and HLSL masks identical.
// Padding covers modest movement of the viewmodel, not every possible weapon.
#define FG_HUD_REGION(x,y) (((x)>=500 && (x)<=1150 && (y)<=86) || ((x)>=1440 && (y)<=45) || ((y)>=862 && (x)<=215) || ((y)>=853 && (x)>=420 && (x)<=570) || ((y)>=875 && (y)<=894 && (x)>=550 && (x)<=1110) || ((y)>=812 && (x)>=780 && (x)<=875) || ((y)>=858 && (x)>=1090 && (x)<=1240) || ((y)>=908 && (x)<=445))
// Envelope of muzzle, receiver, hand and forearm: angled top and left edges.
// Shape uses padded segments, avoids protecting the entire lower-right rectangle.
#define FG_GUN_REGION(x,y) ((y)>=522 && (x)>=955 && ((y)>=760 ? (x)>= (1025-(y-760)*0.42) : (x)>= (1060-((y)-522)*0.15)) && ((x)<=1100 ? (y)>=522 : ((x)<=1325 ? (y)>= (550+((x)-1100)*0.20) : (y)>= (595+((x)-1325)*0.40))))
#define FG_FREEZE_STR_IMPL(x) #x
#define FG_FREEZE_STR(x) FG_FREEZE_STR_IMPL(x)
