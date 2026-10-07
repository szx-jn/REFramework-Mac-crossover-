#include "../src/mods/LooseFileQuarantine.hpp"

#include <cassert>
#include <string>

int main() {
    LooseFileQuarantine quarantine;

    const std::wstring invalid = L"C:\\RE9\\natives\\STM\\LevelDesign\\Item\\UserData\\ItemCatalogUserData.user.3";
    const std::wstring other = L"C:\\RE9\\natives\\STM\\GameAssets\\Content\\MainGame\\MainGame.scn.21";

    assert(!quarantine.contains(invalid));

    quarantine.add(invalid);

    assert(quarantine.contains(invalid));
    assert(!quarantine.contains(other));

    return 0;
}
