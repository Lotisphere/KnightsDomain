// QD Coop — procedural attack motion tests. Pure math, no SDK, no engine.
// No framework: CHECK prints each failure; exit code 0 = all passed, 1 = any failed.
#include "qdcoop/sync/AttackMotion.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace qd::motion;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                              \
    } while (0)

#define CHECK_MSG(cond, ...)                                                           \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);            \
            std::printf(__VA_ARGS__);                                                  \
            std::printf("\n");                                                         \
        }                                                                              \
    } while (0)

void Section(const char* name) { std::printf("[%s]\n", name); }

constexpr WeaponClass kAllClasses[] = {
    WeaponClass::Sword,  WeaponClass::Greatsword, WeaponClass::Dagger, WeaponClass::Spear, WeaponClass::Hammer,
    WeaponClass::Axe,    WeaponClass::Club,       WeaponClass::Scythe, WeaponClass::Fist,  WeaponClass::Gun,
    WeaponClass::Shield, WeaponClass::Tool,       WeaponClass::Unknown,
};

float MaxAbs(const std::vector<BoneOffset>& v) {
    float m = 0;
    for (const BoneOffset& o : v) m = std::fmax(m, std::fmax(std::fabs(o.pitch), std::fmax(std::fabs(o.yaw), std::fabs(o.roll))));
    return m;
}

// Largest per-angle difference between two samples (bones missing on one side count as 0).
float MaxDiff(const std::vector<BoneOffset>& a, const std::vector<BoneOffset>& b) {
    std::vector<BoneOffset> d = a;
    AddScaled(d, b, -1.0f);
    return MaxAbs(d);
}

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool Near(const Vec3& a, const Vec3& b, float eps = 1e-4f) { return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }
bool SameRotation(const Quat& a, const Quat& b, float eps = 1e-4f) {   // q and -q are the same rotation
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    return std::fabs(std::fabs(dot) - 1.0f) <= eps;
}

// ------------------------------------------------------------------------------------------------ tables / sampling

void TestTables() {
    Section("tables");
    std::string err;
    const bool ok = CheckTables(err);
    CHECK_MSG(ok, "%s", err.c_str());
}

void TestSampleEnds() {
    Section("sample: rest pose at t=0 and t=1 (< 1 deg)");
    std::vector<BoneOffset> out;
    for (WeaponClass wc : kAllClasses) {
        for (int v = 0; v < 3; ++v) {
            for (float t : {0.0f, 1e-4f, 1.0f - 1e-4f, 1.0f, -0.5f, 1.5f}) {
                Sample(wc, v, t, out);
                CHECK_MSG(MaxAbs(out) < 1.0f, "%s v%d t=%g max=%g", WeaponClassName(wc), v, t, MaxAbs(out));
            }
        }
    }
}

void TestSampleMoves() {
    Section("sample: every motion actually moves, stays continuous, never touches the pelvis");
    std::vector<BoneOffset> prev, cur;
    for (WeaponClass wc : kAllClasses) {
        for (int v = 0; v < 3; ++v) {
            float peak = 0, worstStep = 0;
            bool pelvis = false;
            Sample(wc, v, 0.0f, prev);
            for (int i = 1; i <= 1000; ++i) {
                const float t = i / 1000.0f;
                Sample(wc, v, t, cur);
                peak = std::fmax(peak, MaxAbs(cur));
                worstStep = std::fmax(worstStep, MaxDiff(cur, prev));
                for (const BoneOffset& o : cur) {
                    if (std::strcmp(o.bone, "Pelvis") == 0) pelvis = true;
                    CHECK(BoneFromName(o.bone) != Bone::None);
                }
                prev = cur;
            }
            CHECK_MSG(peak > 10.0f, "%s v%d peak only %g deg", WeaponClassName(wc), v, peak);
            CHECK_MSG(worstStep < 5.0f, "%s v%d jumps %g deg in one 1/1000 step", WeaponClassName(wc), v, worstStep);
            CHECK_MSG(!pelvis, "%s v%d moves the pelvis", WeaponClassName(wc), v);
        }
    }
}

void TestDurations() {
    Section("durations");
    struct Row { WeaponClass wc; float seconds; };
    const Row rows[] = {
        {WeaponClass::Sword, 0.55f},  {WeaponClass::Greatsword, 0.90f}, {WeaponClass::Dagger, 0.35f},
        {WeaponClass::Spear, 0.60f},  {WeaponClass::Hammer, 0.85f},     {WeaponClass::Club, 0.85f},
        {WeaponClass::Axe, 0.70f},    {WeaponClass::Scythe, 0.80f},     {WeaponClass::Fist, 0.35f},
        {WeaponClass::Gun, 0.40f},    {WeaponClass::Shield, 0.50f},     {WeaponClass::Tool, 0.55f},
        {WeaponClass::Unknown, 0.55f},
    };
    for (const Row& r : rows) {
        for (int v = -1; v <= 4; ++v) {
            const float d = Duration(r.wc, v);
            CHECK_MSG(d > 0.0f, "%s v%d duration %g", WeaponClassName(r.wc), v, d);
            CHECK_MSG(Near(d, r.seconds), "%s v%d duration %g, expected %g", WeaponClassName(r.wc), v, d, r.seconds);
        }
    }
}

void TestVariants() {
    Section("variant mapping");
    auto same = [](WeaponClass a, int va, WeaponClass b, int vb) {
        return std::strcmp(MotionName(a, va), MotionName(b, vb)) == 0;
    };
    CHECK(same(WeaponClass::Greatsword, 2, WeaponClass::Greatsword, 0));
    CHECK(!same(WeaponClass::Greatsword, 1, WeaponClass::Greatsword, 0));
    CHECK(same(WeaponClass::Hammer, 2, WeaponClass::Hammer, 0));
    CHECK(same(WeaponClass::Club, 0, WeaponClass::Hammer, 0));
    CHECK(same(WeaponClass::Club, 1, WeaponClass::Hammer, 1));
    CHECK(same(WeaponClass::Axe, 2, WeaponClass::Axe, 0));
    CHECK(same(WeaponClass::Scythe, 2, WeaponClass::Scythe, 0));
    CHECK(same(WeaponClass::Gun, 1, WeaponClass::Gun, 0) && same(WeaponClass::Gun, 2, WeaponClass::Gun, 0));
    CHECK(same(WeaponClass::Shield, 1, WeaponClass::Shield, 0) && same(WeaponClass::Shield, 2, WeaponClass::Shield, 0));
    for (int v = 0; v < 3; ++v) {
        CHECK(same(WeaponClass::Tool, v, WeaponClass::Sword, v));
        CHECK(same(WeaponClass::Unknown, v, WeaponClass::Sword, v));
    }
    CHECK(!same(WeaponClass::Sword, 0, WeaponClass::Sword, 1) && !same(WeaponClass::Sword, 1, WeaponClass::Sword, 2));
    CHECK(!same(WeaponClass::Dagger, 0, WeaponClass::Dagger, 2) && !same(WeaponClass::Fist, 1, WeaponClass::Fist, 2));
    CHECK(same(WeaponClass::Spear, 3, WeaponClass::Spear, 0));    // modulo 3
    CHECK(same(WeaponClass::Spear, -1, WeaponClass::Spear, 2));   // negative variants wrap too
}

// ------------------------------------------------------------------------------------------------ classification

void TestClassifyFolders() {
    Section("classify: every weapon folder");
    struct Row { const char* folder; WeaponClass expected; };
    using W = WeaponClass;
    const Row rows[] = {
        {"Axe", W::Axe},                       {"B33blade", W::Sword},
        {"BasicSpear", W::Spear},              {"BasicSpearEvolved", W::Spear},
        {"BirdFlail", W::Hammer},              {"BloodyClaw", W::Fist},
        {"BoBlade", W::Sword},                 {"ByrdSword", W::Sword},
        {"Cestus", W::Fist},                   {"CookingPot", W::Hammer},
        {"CrushingLance", W::Spear},           {"CyberHammer", W::Hammer},
        {"CyberSword", W::Sword},              {"Daggers", W::Dagger},
        {"DetestableCLub", W::Club},           {"DiamondBlade", W::Sword},
        {"DuckHammer", W::Hammer},             {"DustyCLub", W::Club},
        {"ElecGun", W::Gun},                   {"EnergyBallHammer", W::Hammer},
        {"EnergySpear", W::Spear},             {"EnergySword", W::Sword},
        {"EyePea_Syringe", W::Dagger},         {"FInalSword", W::Sword},
        {"FinalClaw", W::Fist},                {"FlyingSword", W::Sword},
        {"FlyingSwordEvolved", W::Sword},      {"FlyingSwordTrue", W::Sword},
        {"ForestFlameAxe", W::Axe},            {"GeometrizerBat", W::Hammer},
        {"GhostSpear", W::Spear},              {"GolemArm", W::Hammer},
        {"GreatSpear", W::Spear},              {"HagDagger", W::Dagger},
        {"Halberd", W::Spear},                 {"Hammer", W::Hammer},
        {"HarkLamp", W::Tool},                 {"HarpoClub", W::Spear},          // "Harpo" (Spear) is checked before "Club"
        {"HeavyRifle", W::Gun},                {"HempBLade", W::Sword},
        {"HempieSVT38", W::Gun},               // "SVT" keyword
        {"JarHammer", W::Hammer},
        {"JigekiGMS", W::Gun},                 // "Jigeki" keyword
        {"JosamiMace", W::Hammer},
        {"JosamiSword", W::Sword},             {"Katar", W::Dagger},
        {"KingHammer", W::Hammer},             {"KingSpear", W::Spear},
        {"LunaSpear", W::Spear},               {"Luudecrossbow", W::Gun},
        {"MagicIceSword", W::Sword},           {"ManroomCross", W::Gun},
        {"MorningStar", W::Hammer},            {"OtherWorldSpear", W::Spear},
        {"PandoraBlade", W::Sword},            {"ParryShield", W::Shield},
        {"PebbleHammer", W::Hammer},           {"PlasmaRifle", W::Gun},
        {"PoisonPiercer", W::Dagger},          {"PuwemaLeafBlade", W::Sword},
        {"Rapier", W::Dagger},                 {"Rapier2", W::Dagger},
        {"RayneScythe", W::Scythe},            {"RebornClub", W::Club},
        {"RocketLauncher", W::Gun},            {"RustedGreatAxe", W::Axe},
        {"RustedGreatMorningStar", W::Hammer}, {"RustedGreatSword", W::Greatsword},
        {"RustedLeafBlade", W::Sword},         {"RustedLongHammer", W::Hammer},
        {"RustedSword", W::Sword},             {"RustedSwordEvolved", W::Sword},
        {"SaoSword", W::Sword},                {"SaurianWing", W::Sword},
        {"Scythe", W::Scythe},                 {"Shotgun", W::Gun},
        {"SipehaLongHammer", W::Hammer},       {"SkullRod", W::Tool},
        {"Slashgun", W::Gun},                  {"SniperGun", W::Gun},
        {"Spear", W::Spear},                   {"SpectralSword", W::Sword},
        {"SpikedClub", W::Club},               {"TiniScythe", W::Scythe},
        {"Torch", W::Tool},                    {"TridentKeyTrident", W::Spear},
        {"VampireHammer", W::Hammer},          {"VampireKnife", W::Dagger},
        {"Voxord", W::Sword},                  {"WarHammer", W::Hammer},
        {"WodasoGreatSword", W::Greatsword},   {"WoodClub", W::Club},
        {"WoodClubEvolved", W::Club},          {"YusukeKabob", W::Dagger},
        {"Zerkhander", W::Greatsword},         {"Zweihander", W::Greatsword},
    };
    int n = 0;
    for (const Row& r : rows) {
        ++n;
        const WeaponClass got = ClassifyWeapon(r.folder);
        CHECK_MSG(got == r.expected, "%s -> %s, expected %s", r.folder, WeaponClassName(got), WeaponClassName(r.expected));
        // the same folder inside a realistic item path must give the same answer
        const std::string path = std::string("/Game/Items/Weapons/") + r.folder + "/FS_ItemData." + "FS_ItemData";
        const WeaponClass gotPath = ClassifyWeapon(path);
        CHECK_MSG(gotPath == r.expected, "%s -> %s, expected %s", path.c_str(), WeaponClassName(gotPath),
                  WeaponClassName(r.expected));
    }
    CHECK_MSG(n == 96, "appendix has 96 folders, table has %d", n);
}

void TestClassifyPaths() {
    Section("classify: paths, case, fallbacks");
    CHECK(ClassifyWeapon("") == WeaponClass::Fist);
    CHECK(ClassifyWeapon("/Game/Items/Mystery/FS_ItemData_Thing.FS_ItemData_Thing") == WeaponClass::Sword);
    CHECK(ClassifyWeapon("ZWEIHANDER") == WeaponClass::Greatsword);
    CHECK(ClassifyWeapon("zweihander") == WeaponClass::Greatsword);
    CHECK(ClassifyWeapon("/game/weapons/rapier/da_rapier.da_rapier") == WeaponClass::Dagger);
    // The deepest segment decides: an outer "Combat" folder (contains "bat") must not turn an axe into a hammer.
    CHECK(ClassifyWeapon("/Game/Combat/Weapons/Axe/FS_ItemData_Axe.FS_ItemData_Axe") == WeaponClass::Axe);
    CHECK(ClassifyWeapon("/Game/Combat/Weapons/Generic/ItemData.ItemData") == WeaponClass::Hammer);   // documented limit
    // Asset name wins over the folder when it has a keyword itself.
    CHECK(ClassifyWeapon("/Game/Weapons/Swords/Rapier/Rapier_Data.Rapier_Data") == WeaponClass::Dagger);
    // Sub-object paths (':') and Windows separators.
    CHECK(ClassifyWeapon("/Game/Weapons/Halberd/ID.ID:Sub") == WeaponClass::Spear);
    CHECK(ClassifyWeapon("Weapons\\Shotgun\\ID") == WeaponClass::Gun);
    // Trailing separators do not crash or change the answer.
    CHECK(ClassifyWeapon("/Game/Weapons/Torch/") == WeaponClass::Tool);
    CHECK(ClassifyWeapon("/") == WeaponClass::Sword);
}

// ------------------------------------------------------------------------------------------------ quaternions

void TestQuaternions() {
    Section("quaternions follow UE FRotator semantics");
    const Vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
    CHECK(Near(Rotate(QuatFromRotator(0, 0, 0), X), X));
    CHECK(Near(Rotate(QuatFromRotator(90, 0, 0), X), Z));                // pitch up: forward -> up
    CHECK(Near(Rotate(QuatFromRotator(0, 90, 0), X), Y));                // yaw right: forward -> right
    CHECK(Near(Rotate(QuatFromRotator(0, 0, 90), Y), Vec3{0, 0, -1}));   // roll: right -> down
    CHECK(Near(Rotate(QuatFromRotator(90, 0, 0), Vec3{0, 0, -1}), X));   // a hanging arm pitched +90 points forward
    CHECK(Near(Rotate(QuatFromRotator(0, 0, -90), Vec3{0, 0, -1}), Y));  // right arm roll -90: lifted out to the right
    // FRotator(P, Y, R) = yaw * pitch * roll (roll applied first)
    const Quat composed = Mul(QuatFromRotator(0, 30, 0), Mul(QuatFromRotator(20, 0, 0), QuatFromRotator(0, 0, 10)));
    CHECK(SameRotation(composed, QuatFromRotator(20, 30, 10)));
    // Mul(a, b) applies b first
    const Vec3 v = Rotate(Mul(QuatFromRotator(0, 90, 0), QuatFromRotator(90, 0, 0)), X);   // pitch up, then yaw: still up
    CHECK(Near(v, Z));
    const Vec3 w = Rotate(Mul(QuatFromRotator(90, 0, 0), QuatFromRotator(0, 90, 0)), X);   // yaw to right, then pitch about Y: stays right
    CHECK(Near(w, Y));
    CHECK(SameRotation(Mul(QuatFromRotator(10, 20, 30), Conjugate(QuatFromRotator(10, 20, 30))), Quat{}));
    const Quat n = Normalize(Quat{0, 0, 0, 2});
    CHECK(Near(n.w, 1.0f) && Near(n.x, 0.0f));
    const Quat zero = Normalize(Quat{0, 0, 0, 0});
    CHECK(Near(zero.w, 1.0f));

    Section("character frame -> component space");
    // meshForwardYaw 0: component space == character frame
    const BoneOffset pitchUp{"UpperArm_R", 90, 0, 0};
    CHECK(SameRotation(ToComponentSpace(pitchUp, 0.0f), QuatFromRotator(90, 0, 0)));
    // Knight mesh faces +Y (meshForwardYaw 90): character forward is +Y, character right is -X.
    CHECK(Near(Rotate(ToComponentSpace(pitchUp, 90.0f), Y), Z));                           // forward -> up
    CHECK(Near(Rotate(ToComponentSpace(BoneOffset{"x", 0, 90, 0}, 90.0f), Y), Vec3{-1, 0, 0}));   // yaw right: forward -> right
    CHECK(Near(Rotate(ToComponentSpace(BoneOffset{"x", 90, 0, 0}, 90.0f), Vec3{0, 0, -1}), Y));   // hanging arm -> forward
    CHECK(Near(Rotate(ToComponentSpace(BoneOffset{"x", 0, 0, -90}, 90.0f), Vec3{0, 0, -1}), Vec3{-1, 0, 0}));   // out to the right
    // zero offset is the identity in any frame
    CHECK(SameRotation(ToComponentSpace(BoneOffset{"x", 0, 0, 0}, 90.0f), Quat{}));
}

void TestAddScaled() {
    Section("AddScaled (fade blending)");
    std::vector<BoneOffset> a = {{"Spine_000", 10, 0, 0}};
    const std::vector<BoneOffset> b = {{"Spine_000", 0, 20, 0}, {"UpperArm_R", 40, 0, -10}};
    AddScaled(a, b, 0.5f);
    CHECK(a.size() == 2);
    CHECK(Near(a[0].pitch, 10) && Near(a[0].yaw, 10));
    CHECK(std::strcmp(a[1].bone, "UpperArm_R") == 0 && Near(a[1].pitch, 20) && Near(a[1].roll, -5));
    // name match, not pointer match
    std::string name = "UpperArm_R";
    std::vector<BoneOffset> c = {{name.c_str(), 1, 1, 1}};
    AddScaled(c, b, 1.0f);
    CHECK(c.size() == 2 && Near(c[0].pitch, 41));
}

void TestBones() {
    Section("bone names");
    CHECK(kBoneCount == 16);
    for (int i = 1; i < kBoneCount; ++i) CHECK(BoneFromName(BoneName(static_cast<Bone>(i))) == static_cast<Bone>(i));
    CHECK(BoneFromName("Root") == Bone::None);
    CHECK(std::strcmp(BoneName(Bone::None), "") == 0);
    CHECK(std::strcmp(BoneName(Bone::Wrist_R), "Wrist_R") == 0);
}

}  // namespace

int main() {
    TestTables();
    TestSampleEnds();
    TestSampleMoves();
    TestDurations();
    TestVariants();
    TestClassifyFolders();
    TestClassifyPaths();
    TestQuaternions();
    TestAddScaled();
    TestBones();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
