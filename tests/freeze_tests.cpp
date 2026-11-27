#include "../src/freeze_regions.h"
#include <cassert>
int main(){
    assert(FG_HUD_REGION(825,30));assert(FG_HUD_REGION(1590,12));assert(FG_HUD_REGION(70,890));
    assert(FG_HUD_REGION(480,885));assert(FG_HUD_REGION(825,840));assert(FG_HUD_REGION(1140,880));
    assert(!FG_HUD_REGION(825,460));assert(!FG_HUD_REGION(1500,460));
    assert(FG_GUN_REGION(1080,550));assert(FG_GUN_REGION(1190,650));assert(FG_GUN_REGION(1570,800));assert(FG_GUN_REGION(990,920));
    assert(!FG_GUN_REGION(825,460));assert(!FG_GUN_REGION(1500,540));assert(!FG_GUN_REGION(900,925));
}
