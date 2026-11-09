// Puppets (E2/M3 ghost, M4 partner). A puppet is a second `player` object (Alec Mason, your own h_info) driven through
// capture -> multi_player_state (0xAC) -> add_state(puppet+0x1274) -> multi_game_player_process_state(puppet) every
// gameplay frame. One puppet at a time:
//   ghost   (dev=1 + F6): replays your own captures about 0.4 s late from a local ring. research/notes/ghost_impl.md.
//   partner (a linked session, session.cpp): your captures go out as PSTATE (Puppet_Out); the partner's PSTATEs come in
//           (Puppet_In), are mapped into the local Clock and go through the same ring, Deliver and Effects as the
//           ghost's. research/notes/session_impl.md. F6 is refused while a session is linked.
//
// Installed on the first F6 or link, only if every expected-byte check passes; each hook passes through when no
// puppet exists:
//   0x3EE55C call gamestate_gp_simulate_frame: before = deliver due states, their effects once applied, process_state;
//            after = capture Player
//   0x2FC841 call player_move_set_anim_direct (local movement): records your stick for the capture
//   0x6E62D7 / 0x6E6B36 / 0x6E59E7: player::do_frame, player_process_object_interaction, player_is_killed: not for it
//   0x6CE950 human_damage prologue: the puppet neither takes nor deals damage to humans
//   0x653124 PP-AIR and 0x67ED30 PP-MELEE caves, 0x630B07 / 0x680783 PP-CROUCH thunks
//   0x6C1CD4 call upgrade_get_backpack_type in player::stream: none for a player that is not you (PackTypeThunk)
//   0x59AA78 call camera_shake_stop in multi_object_backpack::deactivate: not for the puppet's pack (G5, ShakeStopThunk)
//   ver6.4 gates on the puppet's pack, policy gc_ppWorld: 0x5F18CA use_ripple cave (G3, NPC / vehicle ripple), 0x5F1942
//   ripple loop thunk (G4, no co-op knockdown), imm32 0x5F3188 ram cast filter (G6), 0x59ABA7 end cutter thunk (G7)
// The partner's pack activity (PSTATE bpAct / bpFuel) runs the full activate / deactivate on the puppet's pack, as an MP
// client does for a remote player (PackEffects; research/notes/backpack_sync.md).
// While a puppet exists: 0x6E6B44 PP-STATS jmp (not with pp_stats=0), 0x3EDDC6 listener store NOPed, 0x654B57 melee turn
// NOPed, MP handle tables primed, hammer 1-3 low-res textures aliased. While a puppet exists or a session is linked: L5
// operands 0x6B19C8 / 0x6CC4A3 ->
// g_coopOne (your own melee and trigger ids must advance for the partner's copy of you).
// The 0x1D24D9 tick (Puppet_Tick) writes Clock_current_time, reads F6 and recreates the puppet after a level load.
#include "features.h"
#include "game.h"
#include "proto.h"
#include <cmath>
#include <cstring>

// Operands of the naked caves below (absolute runtime addresses, set by Install) and their hit counters.
extern "C" {
uint8_t* gc_puppet; // the puppet while it exists (re-resolved every frame), else NULL
uintptr_t gc_mp, gc_player, gc_airMp, gc_airBack, gc_meleeMp, gc_meleeSp, gc_submode, gc_packType, gc_shakeStop;
uintptr_t gc_rippleBack, gc_rippleMovers, gc_humanFromHandle, gc_createObject;
long gc_airHits, gc_meleeHits, gc_crouchSkips, gc_shakeKeeps, gc_rippleSkips, gc_rippleSpared, gc_cutterSkips;
// What the partner puppet's pack may do on this machine (backpack_sync.md §4.3): 1 world (G6 ram cutters on buildings,
// G7 end cutter; G1 / G2 come at M6), 2 NPCs (G3 ripple, G6 ram), 4 vehicles (G6 ram; the ripple's follow the NPC bit,
// G3), 8 live epoch (guest, M6 §6: G6 passes buildings, whose holes come from the host's stream; G7 untouched). Set per
// role by Puppet_Net from kPolicyHost / kPolicyGuest, bit 8 per epoch by Puppet_SetWorldBit.
uint8_t gc_ppWorld = 1;
}

namespace {

namespace site {
constexpr uintptr_t SimFrameCall   = 0x003EE55C; // gamestate_gp_process: E8 -> gamestate_gp_simulate_frame, void __cdecl()
constexpr uintptr_t SimFrame       = 0x003EDB60;
constexpr uintptr_t ListenerStore  = 0x003EDDC6; // ...simulate_frame: 89 6C 24 10 mov [esp+0x10],ebp = LAST player (below)
constexpr uintptr_t AnimDirectCall = 0x002FC841; // process_player_normal_movement_internal (local player only): E8 ->
constexpr uintptr_t AnimDirect     = 0x0069CFA0; //   void __cdecl player_move_set_anim_direct(player*, float side, float fwd, bool sprint, bool)
constexpr uintptr_t DoFrameCall    = 0x006E62D7; // player_do_frame: mov ecx,esi / E8 -> player::do_frame (thiscall, void)
constexpr uintptr_t DoFrame        = 0x006D5660;
constexpr uintptr_t InteractCall   = 0x006E6B36; // player_do_frame non-local path: E8 -> player_process_object_interaction
constexpr uintptr_t Interact       = 0x006DE050; //   void __cdecl(player*)
constexpr uintptr_t KilledCall     = 0x006E59E7; // human_do_frame: E8 -> player_is_killed (its only caller)
constexpr uintptr_t Killed         = 0x00634360; //   void __cdecl(player*)
constexpr uintptr_t StatsSkip      = 0x006E6B44; // PP-STATS: 8B CE -> EB 5E (jmp 0x6E6BA4, stack balanced)
constexpr uintptr_t HumanDamage    = 0x006CE950; // float __cdecl(human*, human*, float, int, weapon_info*, object*, bool, bool)
constexpr uintptr_t L5Melee        = 0x006B19C6; // human_melee_attack: 80 3D <Multiplayer> 00 74 79 (bump melee ids)
constexpr uintptr_t L5Trigger      = 0x006CC4A1; // human_weapons_firearm_pull_trigger_common: 80 3D <Multiplayer> 00 74 32
constexpr uintptr_t AirSite        = 0x00653124; // human_process_controllers_pre_integrate: A0 <Multiplayer> 84 C0 74 41
constexpr uintptr_t AirBack        = 0x00653129; //   test al,al / je 0x65316E (local stick)
constexpr uintptr_t AirMp          = 0x0065315C; //   MP read of the human's own +0x1180 / +0x117C
constexpr uintptr_t MeleeSite      = 0x0067ED30; // human_combat_play_combat_move: 80 3D <Multiplayer> 00 74 2B
constexpr uintptr_t MeleeMp        = 0x0067ED39; //   cmp eax,edi: heading from the attacker's +0x1250
constexpr uintptr_t MeleeSp        = 0x0067ED64; //   heading from Rfg_camera+0x68 (the observer's view)
// human_turn_towards_melee_target (each frame of a swing): `je 0x654C85` (0F 84 28 01 00 00) sends every player-subtype
// human in SP to the camera heading. NOPed while a puppet exists: the next `cmp esi,[Player] / je 0x654C85` keeps you on
// the camera path and the puppet takes the MP path (its own +0x1250, 0x654B69). Same leak as PP-MELEE (audit wf_2c6ffee5).
constexpr uintptr_t MeleeTurn      = 0x00654B57;
constexpr uintptr_t CrouchCall     = 0x00630B07; // human_move_start_crouching: E8 -> camera_free_set_submode (esi = human)
constexpr uintptr_t StandCall      = 0x00680783; // human_move_stop_crouching: same
constexpr uintptr_t SetSubmode     = 0x002CB2F0; //   void __cdecl(free_camera_submodes, float, float)
// cartographer::collect_humans (minimap, every frame) treats every ALLIED / FRIENDLY human that is not you as an npc:
// object_cast<npc> returns NULL for the puppet and it reads [NULL+0xD00] (0x46D3EA, the ver2 crash) or [NULL+0xB4]
// (0x46D43D). Vanilla never gets there with NULL: you are skipped first (0x46D29F) and every other human is an npc.
constexpr uintptr_t MapAllied      = 0x0046D3CF; // je 0x46D3D9 (74 08) -> je 0x46D387 (74 B6): not an npc = next human
constexpr uintptr_t MapFriendly    = 0x0046D43E; // test [edi+0xB4],0x8000: ModRM 87 -> 86 = [esi+0xB4], the human itself
constexpr uintptr_t ReservedHash   = 0x01290C60; // Reserved_network_to_game_hash: +0x40 buckets, +0x44 size (0 in SP)
constexpr uintptr_t RopHash        = 0x01290B88; // Network_handle_to_rop_hash: same layout
// The game -> network direction (ver6 two-PC crash, C0000094 in uint_hash's div 0x18FD00 at size 0): a puppet's movement
// correction raycast -> havok_game_filter::isCollisionEnabled -> collide_obstructions 0x5CE126 ->
// multi_core_network_handle_from_game_handle 0x5B6401 (this table), whose miss falls to
// multi_core_get_remote_object_from_game_handle 0x5B6340 (Game_handle_to_rop_hash). Same layout, primed the same way.
constexpr uintptr_t GameNetHash    = 0x01290C18; // Reserved_game_to_network_hash
constexpr uintptr_t GameRopHash    = 0x01290B40; // Game_handle_to_rop_hash
constexpr uintptr_t Clock          = 0x02BEA538; // Clock_current_time, unsigned ms; frozen in SP
constexpr uintptr_t RfgCamera      = 0x019E3B50; // rfg_camera: +0x50 real_orient (the aim, as MP's sender at 0x2FC5B8)
constexpr uintptr_t HudMessageAdd  = 0x00455D30; // hud_message_handle __cdecl(char*, float, void*, void*, int*)
constexpr uintptr_t ObjFromHandle  = 0x0053BEE0; // object* __thiscall world::object_from_handle(unsigned)
constexpr uintptr_t WreckingCrew   = 0x003603B0; // bool __cdecl wrecking_crew_is_active()
// creation, as player_multi_registration::process_create_packet 0x5E0657-0x5E0767 (MP-only props skipped)
constexpr uintptr_t HumanProps     = 0x006529A0; // obj_human_props::obj_human_props(human_info*, vector*, matrix*, char*), thiscall, ret 0x10
constexpr uintptr_t ClassHash      = 0x00610140; // unsigned __cdecl player::get_classname_hash()
constexpr uintptr_t CreateObject   = 0x0053E550; // object* __thiscall world::create_object_internal(unsigned, obj_prop_block*, world_zone*, unsigned)
constexpr uintptr_t SetState       = 0x00607520; // void __cdecl human_set_state(human*, human_states, human_movement_states, float)
constexpr uintptr_t SetZoom        = 0x006851A0; // void __cdecl player_set_zoom_state(player*, player_zoom_state, bool)
constexpr uintptr_t SetVisible     = 0x0068C070; // void __cdecl object_set_visibility_plus_children(object*, bool)
constexpr uintptr_t PropsDtor      = 0x005217D0; // void __thiscall rfg_prop_block::~rfg_prop_block()
constexpr uintptr_t SimReset       = 0x00568B20; // void __thiscall multi_sim_state<multi_player_state>::reset()
constexpr uintptr_t AddState       = 0x00568B40; // void __thiscall ...::add_state(multi_player_state*), ret 4
constexpr uintptr_t ObtainState    = 0x00568C20; // bool __thiscall ...::obtain_state(unsigned time, multi_player_state*, bool*), ret 0xC
constexpr uintptr_t ProcessState   = 0x005E1C50; // void __cdecl multi_game_player_process_state(player*)
constexpr uintptr_t Shutdown       = 0x0069DCA0; // void __cdecl player_level_shutdown(player*): ragdoll unbind, squads, world::destroy_object
// capture
constexpr uintptr_t GetWeapon      = 0x0065A120; // weapon* __cdecl human_weapons_get_weapon(human*)
constexpr uintptr_t TriggerPulled  = 0x0065A5F0; // bool __cdecl human_weapons_is_trigger_pulled(human*): weapon+0x18C bit 1
constexpr uintptr_t IsReloading    = 0x0065A380; // bool __cdecl human_weapons_is_reloading(human*, bool)
constexpr uintptr_t IsZoomed       = 0x006106C0; // bool __cdecl player_is_zoomed(player*)
constexpr uintptr_t IsCrouching    = 0x0060B040; // bool __cdecl human_move_is_crouching(human*)
constexpr uintptr_t RagdollPos     = 0x0041CBA0; // void __cdecl havok_ragdoll_get_pos(int, vector*, ragdoll_part)
constexpr uintptr_t RagdollVel     = 0x0041CC20; // void __cdecl havok_ragdoll_get_vel(int, vector*)
constexpr uintptr_t TimeUntil      = 0x0018E640; // int __thiscall timestamp::time_until()
constexpr uintptr_t MannedTurret   = 0x00655320; // turret* __cdecl human_combat_get_manned_turret(human*), NULL if none
constexpr uintptr_t LadderRung     = 0x00636880; // int __cdecl ladder_compute_current_rung(human*, unsigned ladder_handle)
// remote charges, as MP's read_control_flags does for a remote player (0x5DD6AF / 0x5DD713)
constexpr uintptr_t DetonateCharges = 0x006C51E0; // bool __cdecl detonate_charges(unsigned owner_handle)
constexpr uintptr_t DetonateInAir  = 0x006E4DE0; // bool __cdecl detonate_in_air(unsigned owner_handle)
// weapon switch, as apply_compressed_state 0x5F95A2-0x5F9606
constexpr uintptr_t ItemInSlot     = 0x00612330; // inventory_item* __cdecl inv_item_get_item_in_slot(human*, int)
constexpr uintptr_t AddItem        = 0x006B4E20; // inventory_item* __cdecl human_add_item_to_inventory(human*, inv_item_info*, int, int, int, bool, bool, bool)
constexpr uintptr_t DeleteItem     = 0x006C76A0; // void __cdecl human_delete_item_from_inventory(human*, inventory_item*, bool, bool)
constexpr uintptr_t EquipItem      = 0x006C2770; // bool __cdecl player_equip_item(player*, inventory_item*, item**, bool)
// partner (M4)
constexpr uintptr_t InvItemInfo    = 0x02C6D550; // inv_item_info Inv_item_info[64], stride 0x28, +0x04 handle (inv_item_info_from_handle 0x612370)
constexpr uintptr_t NumInvItemInfo = 0x02C2E0E4; // int Num_inv_item_info
constexpr uintptr_t ZoneStreamed   = 0x0052E020; // bool __thiscall world::is_zone_streamed(vector*), ret 4 (call site 0x6E6190)
constexpr uintptr_t Cartographer   = 0x027B61F8; // cartographer (static object)
// void __thiscall cartographer::add_temporary_icon(vector* pos, minimap_elements bitmap, bool stick, unsigned handle,
// color*, bool blink, unsigned activity_handle), ret 0x1C (map_icon_temp fields, PDB). Call sites 0x369F37 / 0x3820AF /
// 0x39AEEF: push activity handle (-1 ok), 0, &Default_map_item_color, -1, stick, element, &pos; mov ecx,Cartographer.
// Copies x/z into its per-frame list (+0x11974); cartographer::render empties it after drawing (0x4830A5) and
// cartographer::process refuses adds while the game is paused (Add_temp_icons = 0, 0x47CF14).
constexpr uintptr_t AddTempIcon    = 0x00473FF0;
constexpr uintptr_t DefaultMapColor = 0x012611A0; // color Default_map_item_color: FF FF FF FF in the exe, never written
constexpr int StarGreen            = 2;          // MAP_ELEMENT_STAR_GREEN_ICON (ally squads use 1, STAR_BLUE, at 0x633C49)
// partner gear (ver6.2): research/findings/workflows/puppet_backpack.md, puppet_item_textures.md
constexpr uintptr_t StreamPackCall = 0x006C1CD4; // player::stream (esi = this, 0x6C1C87): E8 -> upgrade_get_backpack_type
constexpr uintptr_t PackType       = 0x00375CE0; //   int __cdecl(): YOUR table (player_get_meta_data 0x610550 reads Player)
constexpr uintptr_t PackCreate     = 0x0059AC50; // multi_object_backpack* __cdecl multi_game_backpack_create(int id, vector*, matrix*)
constexpr uintptr_t PackPickedUp   = 0x005F23D0; // void __thiscall multi_object_backpack::event_player_picked_up(player*, bool, bool), ret 0xC
constexpr uintptr_t PackDropped    = 0x005DF1C0; // void __thiscall multi_object_backpack::event_player_dropped(bool), ret 4
constexpr uintptr_t DestroyObject  = 0x0051CEF0; // bool __thiscall world::destroy_object(object*, bool), ret 8 (as 0x5DF2B0)
constexpr uintptr_t BmLoad         = 0x001CA450; // int __cdecl bm_load(const char* name, bool dbg): handle; an unknown name gets a placeholder
constexpr uintptr_t BmBitmaps      = 0x018D194C; // Bm_bitmaps: 12-byte entries, +4 peg entry (+0x10 frames, +0x28 device texture)
constexpr uintptr_t BogusPegEntry  = 0x018E7654; // Bogus_static_bitmap_peg_entry: every placeholder's +4 (bm_init 0x1D07AF, bm_add_bitmap 0x1C9B83)
// partner pack effects (ver6.3): research/notes/backpack_sync.md §4
constexpr uintptr_t PackActivate   = 0x005F2030; // void __thiscall multi_object_backpack::activate(), plain ret (0x5F23C0)
constexpr uintptr_t PackDeactivate = 0x0059AA30; // void __thiscall multi_object_backpack::deactivate(), ret (0x59AC4E); no
                                                 //   "is active" guard (0x59AA46 only picks the ability block)
constexpr uintptr_t ShakeStopCall  = 0x0059AA78; // deactivate, deactivatable packs (esi = the pack): E8 -> camera_shake_stop
constexpr uintptr_t ShakeStop      = 0x002C6F40; //   void __cdecl(): zeroes all five Cur_camera_shake slots (eax, ecx only)
// partner pack gates (ver6.4): backpack_sync.md §4.3
constexpr uintptr_t RippleGate     = 0x005F18CA; // use_ripple: 38 1D <MP> cmp [Multiplayer],bl; 0F 85 B6 03 00 00 jne 0x5F1C8C
constexpr uintptr_t RippleGateBack = 0x005F18D0; //   that jne (kept)
constexpr uintptr_t RippleMovers   = 0x005F1E7B; //   object-mover impulses only: the MP client's path (0x5F1C93)
constexpr uintptr_t RippleHumanCall = 0x005F1942; // SP ripple loop (ebp = the pack): E8 -> world::object_from_handle<human>
constexpr uintptr_t HumanFromHandle = 0x001F88C0; //   human* __thiscall(world*, unsigned), ret 4, NULL if not a human
constexpr uintptr_t RamFilterImm   = 0x005F3184; // process: C7 44 24 7C <imm32> mov [esp+0x7C], the ram spherecast filter
constexpr uintptr_t RhinoFilter    = 0x0057BF50; //   rhino_cast_filter_callback: bool __cdecl(object*, data*), data+0 wearer
constexpr uintptr_t EndCutterCall  = 0x0059ABA7; // deactivate (esi = the pack): E8 -> world::create_object_internal (ret 0x10)
constexpr uintptr_t HumanInAir     = 0x00653A20; // bool __cdecl human_is_in_air(human*) (activate's Thrust test, 0x5F227D)
} // namespace site

struct vector { float x, y, z; };
struct matrix { vector r, u, f; };

// multi_player_state (PDB), what add_state / obtain_state copy as 0x2B dwords
struct MpState {
    vector pos, vel, angVel;      // +0x00 +0x0C +0x18
    matrix orient;                // +0x24
    uint32_t time;                // +0x48 network_time (Clock_current_time domain)
    matrix camOrient;             // +0x4C the aim
    float camAngVel[2];           // +0x70
    float forward, sideways;      // +0x78 +0x7C
    float hpPct;                  // +0x80
    uint32_t ladder;              // +0x84 ladder handle
    int ragdoll, knockdown;       // +0x88 human_ragdoll_state, +0x8C ms until the knockdown ends
    uint16_t turretNet, pad0;     // +0x90 network_to_game_handle: net id (0xFFFF none)
    uint32_t turretGame;          // +0x94   cached game handle
    uint16_t vehNet, pad1;        // +0x98
    uint32_t vehGame;             // +0x9C
    uint8_t seat, pad2;           // +0xA0
    uint16_t ladderState;         // +0xA2
    uint32_t bits;                // +0xA4 bitfield (puppet.md §9)
    uint16_t magazine, reserve;   // +0xA8 +0xAA
};
static_assert(sizeof(MpState) == 0xAC, "multi_player_state");

constexpr uint32_t kDelayMs = 300, kInterpMs = 100; // ghost delivery delay; interpolation delay (ghost and partner)

// at: capture time (sender's Clock); due: Clock time for add_state (Deliver); st.time: network_time, the Clock time
// process_state applies it, which is also when Effects runs. Ghost: due = at + 300, time = at + 400. Partner: due =
// arrival, time = the sender's capture time mapped into the local Clock + 100 (MapTime).
// info: inv_item_info* of the equipped item; pack: worn backpack, Backpack_info id + 1, 0 = none; bp, fuel: its activity
// as PSTATE bpAct / bpFuel (Capture), except that the receiver (Puppet_In) turns the counter in bits 0-3 into kEdge / kStale.
// kFrozen (bpAct bit 7): a still state (Puppet_Out: you are paused, or nothing captured for 150 ms): its level is stale
// (its fuel is not for the Jetpack: a paused tank does not change). The Esc menu (state 15) keeps the sim frame and so
// Capture running (ver6.3 test), so "paused" comes from session.cpp's pause test, not from missing captures.
struct Snap { MpState st; uint32_t at, due; void* info; bool visible; uint8_t pack, bp, fuel; };
// kStale: a counter step first seen after your menu, map or load (Puppet_In skipped states), in a state 250 ms+ late (or
// due after 250 ms without a puppet frame), or after a loss burst of 5+ states: it acts as a resync instead. kAway: the
// first state after your menu, map or load, or such a late state; its level 0 ends the pack quietly (the puppet still stands where it was: no end hole or dust there)
enum : uint8_t { kEdge = 1, kStale = 2, kAway = 4, kLevel = 0x10, kUp = 0x20, kLanded = 0x40, kFrozen = 0x80 }; // Snap::bp on the receiver
// ponytail: an allowlist per phase. P1 activates Fleetfoot 2, Stealth 3, Vision 4, Commando 5, Heal 7, Tremor 9 (the
// Jetpack, id 0, gets its fuel only); P2 adds Thrust 1, Shockwave 6, Rhino 8 with their gates (backpack_sync.md §6)
constexpr unsigned kPackAllow = 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6 | 1u << 7 | 1u << 8 | 1u << 9;
// gc_ppWorld per role. "Now" (P1, P2; Q7 / Q19): the world bit on both sides (parallel worlds, as the puppet's charges),
// no NPC or vehicle hits. M6 (ver7.1): the guest's world bit follows each epoch's mode (Puppet_SetWorldBit: bit 8 set in
// a live epoch, whose holes come from the host's stream: G6 passes buildings, G7 still makes the end cutter, whose holes
// are queue events Replay drops; clear in mode 0 or a stale epoch); the host keeps 1. M7a: the host adds 2; M8: 4.
constexpr uint8_t kPolicyHost = 1, kPolicyGuest = 1;
Snap g_ring[512];
int g_head, g_count, g_sent; // g_sent: entries from g_head already passed to add_state (their effects still pending)
Snap g_last;      // latest on-foot capture (the stop state while you drive is built from it)

bool g_dev, g_l5 = true, g_ppStats = true, g_installed, g_on, g_f6, g_driving, g_seeded, g_visible = true, g_sentVisible = true;
bool g_net;       // a session is linked (session.cpp): the puppet is the partner, your captures go out as PSTATE
bool g_isPartner; // kind of the puppet that exists (or is being created): partner, else ghost
bool g_icon = true; // ini partner_icon: a minimap star at the partner's real position
float g_offset;   // metres along world X (ini ghost_offset)
unsigned g_handle = ~0u;
uint32_t g_lastTime, g_delivered;
void* g_info = (void*)1; // inv_item_info* last given to the puppet (1 = none yet)
int g_slot = -1;
uint8_t g_pack;          // Snap::pack the puppet wears; 0 = none, which every new puppet wears (PackTypeThunk)
uint8_t g_coopOne = 1;   // L5 operand target
uint32_t g_zeroBucket;   // the primed tables' single empty bucket
long long g_qpc0, g_qpcFreq;
double g_lastStat;
// capture side
float g_fwd, g_side;
bool g_moveSeen, g_prevPulled;
int g_down, g_prevC4Down;
uint8_t* g_capPlayer;      // the Player the capture baselines belong to (a level load makes a new one)
// partner: sender side (Capture -> Puppet_Out)
Snap g_out;                // latest capture for PSTATE (the stop state while you drive)
vector g_outWhere;         // your real position (your vehicle while you drive)
bool g_outFresh;           // captured since the last PSTATE
uint8_t g_sentBp;          // bpAct of the last PSTATE (Puppet_Step)
bool g_bpRepeat;           // the last PSTATE stepped the counter with level 1: the next one repeats the level (Puppet_Out)
uint8_t g_detCharges, g_detAir; // detonation edge counters (a one-frame edge must survive the 20 Hz sampling)
uint8_t* g_bpObj;          // your worn pack at the last capture, its activated_time end tick, the activation counter
uint32_t g_bpTick;
uint8_t g_bpCount;
// partner: receiver side (Puppet_In)
struct { bool have; int32_t off, target, winMin; uint32_t winStart; } g_map; // sender Clock -> local Clock
Snap g_lastVis;            // last visible partner state (a hidden puppet stands still there)
uint32_t g_lastVisAt;      // Clock when it arrived (0 = none since the link)
bool g_lastInVisible, g_partnerLoading, g_detHave, g_bpHave;
bool g_bpAway;             // Puppet_In skipped states (your menu, map, load): the next counter step may be stale
uint32_t g_simAt;          // Clock of the puppet's last process_state (BeforeSim)
uint8_t g_detSeen, g_bpSeen;
// the next visible state activates a pack the partner has on without an edge: a new puppet, a relink, a partner load, a
// pack change, back from hidden (backpack_sync.md §3.3)
bool g_bpResync = true, g_jetLogged;
vector g_partnerWhere;     // the partner's real position (F7)
uint32_t g_partnerAt;      // Clock when it arrived (0 = none)
int g_nBad, g_nItem, g_createFails;

uintptr_t g_simNext, g_animNext, g_doFrameNext, g_interactNext, g_killedNext;
using DamageFn = float(__cdecl*)(uint8_t*, uint8_t*, float, int, void*, void*, int, int);
DamageFn g_damageNext;

struct Table { uintptr_t at; uint32_t buckets, size; bool primed; } g_tables[4] = {
    {site::ReservedHash}, {site::RopHash}, {site::GameNetHash}, {site::GameRopHash}};
// Hammers 1-3's low-res meshes name textures the items peg spells sledge_up_N_d_low / _s_low (names compare without
// case): bm_load gave them the bogus placeholder. Only your own held weapon streams '_high' (player_do_frame local branch
// 0x6E6989), so the puppet's hammer shows them: white. Spec-only gaps (hammers 4-7, golden) are left: barely visible.
struct Alias { const char *bad, *good; int h; void* was; } g_alias[6] = {
    {"sledge_up_1_low_d.tga", "sledge_up_1_d_low.tga", -1}, {"sledge_up_1_low_s.tga", "sledge_up_1_s_low.tga", -1},
    {"sledge_up_2_low_d.tga", "sledge_up_2_d_low.tga", -1}, {"sledge_up_2_low_s.tga", "sledge_up_2_s_low.tga", -1},
    {"sledge_up_3_low_d.tga", "sledge_up_3_d_low.tga", -1}, {"sledge_up_3_low_s.tga", "sledge_up_3_s_low.tga", -1}};

inline uint8_t* LocalPlayer() { return *Rva<uint8_t*>(rva::Player); }
inline bool Mp() { return *Rva<uint8_t>(rva::Multiplayer); }
inline uint32_t ClockNow() { return *Rva<uint32_t>(site::Clock); }
inline bool IsPuppet(const uint8_t* p) { return p && p == gc_puppet && p != LocalPlayer(); }
uint8_t* ObjectFromHandle(unsigned h) { return Fn<uint8_t* __thiscall(void*, unsigned)>(site::ObjFromHandle)(Rva(rva::World), h); }
void Hud(const char* text) { Fn<int __cdecl(char*, float, void*, void*, int*)>(site::HudMessageAdd)((char*)text, 3.0f, nullptr, nullptr, nullptr); }
const char* Who() { return g_isPartner ? "partner" : "ghost"; } // log prefix
void Note(const char* what, int& n) { if (n < 3) Log("%s: %s for the puppet (#%d)", Who(), what, ++n); } // first few only

// ---- hooks ---------------------------------------------------------------------------------------------------------
int g_nDoFrame, g_nInteract, g_nKilled, g_nVictim, g_nAttacker, g_nRagdoll;

void __thiscall OnPlayerDoFrame(uint8_t* p) { // backpack theft, radiation, district HUD/radio treat any player as local
    if (IsPuppet(p)) return Note("player::do_frame skipped", g_nDoFrame);
    ((void(__thiscall*)(uint8_t*))g_doFrameNext)(p);
}
void __cdecl OnInteract(uint8_t* p) { // the puppet would eat pickups (object_item_player_touch)
    if (IsPuppet(p)) return Note("player_process_object_interaction skipped", g_nInteract);
    ((void(__cdecl*)(uint8_t*))g_interactNext)(p);
}
void __cdecl OnKilled(uint8_t* p) { // second guard after never_die: no 4 s death timer, no SP death branch
    if (IsPuppet(p)) return Note("player_is_killed skipped", g_nKilled);
    ((void(__cdecl*)(uint8_t*))g_killedNext)(p);
}
void __cdecl OnAnimDirect(uint8_t* p, float side, float fwd, int sprint, int stopped) { // SP never stores the stick
    if (p == LocalPlayer()) g_side = side, g_fwd = fwd, g_moveSeen = true;          // in +0x117C/+0x1180 (MP-only 0x2FC5A8)
    ((void(__cdecl*)(uint8_t*, float, float, int, int))g_animNext)(p, side, fwd, sprint, stopped);
}
// Puppet rules (RESEARCH.md §4.6; the ghost, and the partner in M4's parallel worlds): EDF can shoot it and it never
// dies; its shots are visible but harmless to humans.
float __cdecl OnHumanDamage(uint8_t* victim, uint8_t* attacker, float dmg, int type, void* wi, void* obj, int b1, int b2) {
    Destr_Crush(victim, attacker, type); // M6 crush log for M7a (dev=1, counts only)
    if (IsPuppet(victim)) { Note("damage to it swallowed", g_nVictim); return 0.0f; }
    if (IsPuppet(attacker)) { Note("damage by it swallowed", g_nAttacker); return 0.0f; }
    // human_damage 0x6CE964-0x6CE9B1: with no attacker, a victim in ragdoll state not 0/2/3 (4 only with +0xBC bit 15) is
    // charged to ragdoll_source_human_handle +0x340, set by human_ragdoll_start 0x6C97AF from the knockdown's source (the
    // puppet's explosions 0x6DC366 / 0x6DC451, its knockdown swings 0x6D1130). Its impact and get-up damage arrive with a
    // NULL attacker (havok_collide_ragdoll 0x4259E6, human_ragdoll_get_up 0x6D4FBF). Victim NULL: the original's check.
    // dmg < 0 = the game's own forced kill (all current HP: comiss 0x6CED66 -> 0x6CEDDB): kill regions (trigger_region
    // 0x6D64A1 / 0x6D6505), the below-world phantom (0x43BF69, fires once), maybe_kill_player 0x37F8D2, other mods'
    // suicide / kill. Not the puppet's damage: passed on (the original credits it to the puppet, harmless in SP).
    if (!attacker && victim && gc_puppet && dmg >= 0.0f) {
        int rs = Field<int>(victim, 0x33C);
        if (rs != 0 && rs != 2 && rs != 3 && (rs != 4 || (Field<uint32_t>(victim, 0xBC) & 0x8000)) &&
            Field<unsigned>(victim, 0x340) == g_handle && IsPuppet(gc_puppet)) {
            Note("ragdoll damage by it swallowed", g_nRagdoll);
            return 0.0f;
        }
    }
    return g_damageNext(victim, attacker, dmg, type, wi, obj, b1, b2);
}

// PP-AIR (replaces `mov al,[Multiplayer]` at 0x653124): the airborne puppet steers with its own replicated command
// (+0x1180 / +0x117C, written by apply_movement_state), not with the local stick. Anyone else, or MP: the original flow
// with al = Multiplayer.
__attribute__((naked)) void AirCave() {
    asm volatile("pushl %ecx\n\t"
                 "movl _gc_mp, %ecx\n\t"
                 "movb (%ecx), %al\n\t"
                 "popl %ecx\n\t"
                 "testb %al, %al\n\t"
                 "jne 1f\n\t"
                 "cmpl _gc_puppet, %esi\n\t"
                 "jne 1f\n\t"
                 "incl _gc_airHits\n\t"
                 "jmp *_gc_airMp\n"
                 "1:\n\t"
                 "jmp *_gc_airBack");
}
// PP-MELEE (replaces `cmp byte [Multiplayer],0 / je` at 0x67ED30): eax = the acting human if player subtype, else 0.
// The puppet's lunge heads along its own +0x1250 (the MP path); you and NPCs keep the camera path. ecx is free: both
// targets overwrite it first. MP: the original MP path.
__attribute__((naked)) void MeleeCave() {
    asm volatile("movl _gc_mp, %ecx\n\t"
                 "cmpb $0, (%ecx)\n\t"
                 "jne 2f\n\t"
                 "testl %eax, %eax\n\t"
                 "je 1f\n\t"
                 "cmpl _gc_puppet, %eax\n\t"
                 "jne 1f\n\t"
                 "incl _gc_meleeHits\n"
                 "2:\n\t"
                 "jmp *_gc_meleeMp\n"
                 "1:\n\t"
                 "jmp *_gc_meleeSp");
}
// PP-CROUCH (call sites 0x630B07 / 0x680783, esi = the human crouching or standing): your free camera switches its
// crouch/stand framing only for your own crouch. Cdecl, args untouched: tail-jump or plain ret.
__attribute__((naked)) void CrouchThunk() {
    asm volatile("pushl %eax\n\t"
                 "movl _gc_player, %eax\n\t"
                 "cmpl (%eax), %esi\n\t"
                 "popl %eax\n\t"
                 "jne 1f\n\t"
                 "jmp *_gc_submode\n"
                 "1:\n\t"
                 "incl _gc_crouchSkips\n\t"
                 "ret");
}
// player::stream 0x6C1CD4 (SP non-WC only: 0x6C1CBA / 0x6C1CC7): the type comes from YOUR table and 0x6C1CF2-0x6C1D18
// make that pack for whoever streams in, then store s_lastBackpackIndex. Not you while you exist (the puppet, whenever
// it streams) -> 0x3E = none (0x6C1CD9 jumps to the armor). Player 0 (a level's own player being created:
// player_level_shutdown_all 0x69DD66 .. init_post 0x6D5A83) or you: the original. Only eax (the result) changes.
__attribute__((naked)) void PackTypeThunk() {
    asm volatile("movl _gc_player, %eax\n\t"
                 "movl (%eax), %eax\n\t"
                 "testl %eax, %eax\n\t"
                 "je 1f\n\t"
                 "cmpl %eax, %esi\n\t"
                 "je 1f\n\t"
                 "movl $0x3E, %eax\n\t"
                 "ret\n"
                 "1:\n\t"
                 "jmp *_gc_packType");
}
// G5, deactivate 0x59AA78 (esi = the pack): camera_shake_stop clears all of YOUR current shakes. Not when the pack is the
// puppet's (+0x274 wearer; a swap's event_player_dropped deactivates before it clears +0x274, 0x5DF1FE). Cdecl, no args,
// result unused; eax is free (the callee clobbers it): tail-jump or plain ret.
__attribute__((naked)) void ShakeStopThunk() {
    asm volatile("movl _gc_puppet, %eax\n\t"
                 "testl %eax, %eax\n\t"
                 "je 1f\n\t"
                 "cmpl 0x274(%esi), %eax\n\t"
                 "jne 1f\n\t"
                 "incl _gc_shakeKeeps\n\t"
                 "ret\n"
                 "1:\n\t"
                 "jmp *_gc_shakeStop");
}
// G3, use_ripple 0x5F18CA (replaces `cmp byte [Multiplayer],bl`; the `jne 0x5F1C8C` after it stays). ebp = the pack,
// ebx = 0, esi = the position, edi = the ability set: the state an MP client brings to 0x5F1E7B (from 0x5F1C93, same
// stack). The puppet's ripple without the NPC bit goes there, to the object-mover impulses only: no NPC or vehicle hit.
// Anyone else: the replaced compare (flags for the jne), eax kept.
__attribute__((naked)) void RippleCave() {
    asm volatile("pushl %eax\n\t"
                 "movl _gc_puppet, %eax\n\t"
                 "testl %eax, %eax\n\t"
                 "je 1f\n\t"
                 "cmpl 0x274(%ebp), %eax\n\t"
                 "jne 1f\n\t"
                 "testb $2, _gc_ppWorld\n\t"
                 "jne 1f\n\t"
                 "popl %eax\n\t"
                 "incl _gc_rippleSkips\n\t"
                 "jmp *_gc_rippleMovers\n"
                 "1:\n\t"
                 "movl _gc_mp, %eax\n\t"
                 "cmpb %bl, (%eax)\n\t"
                 "popl %eax\n\t"
                 "jmp *_gc_rippleBack");
}
// G4, the SP ripple loop's call 0x5F1942 to world::object_from_handle<human> (thiscall, ecx = World, one arg, ret 4;
// ebp = the pack): you or the puppet comes back as the wearer, which the loop's `cmp esi,wearer / je` (0x5F194F) skips.
// No co-op knockdown by any ripple, both directions (MP's teammate rule, 0x5F1CF8). Only players wear packs, so the
// wearer is you or the puppet. ecx / edx are the callee's anyway. Always on while a puppet exists.
__attribute__((naked)) void RippleHumanThunk() {
    asm volatile("pushl 4(%esp)\n\t"
                 "call *_gc_humanFromHandle\n\t"
                 "movl _gc_puppet, %edx\n\t"
                 "testl %edx, %edx\n\t"
                 "je 2f\n\t"
                 "cmpl %edx, %eax\n\t"
                 "je 1f\n\t"
                 "movl _gc_player, %ecx\n\t"
                 "cmpl (%ecx), %eax\n\t"
                 "jne 2f\n"
                 "1:\n\t"
                 "cmpl 0x274(%ebp), %eax\n\t"
                 "je 2f\n\t"
                 "movl 0x274(%ebp), %eax\n\t"
                 "incl _gc_rippleSpared\n"
                 "2:\n\t"
                 "ret $4");
}
// G7, deactivate 0x59ABA7 (esi = the pack): world::create_object_internal (thiscall, 4 args, ret 0x10, result unused,
// 0x59ABAC) makes the end-of-charge cutter of a Rhino or Thrust. The puppet's pack without bit 0 (a quiet end): none;
// a live epoch (bit 8) keeps it, as its holes are queue events Replay drops. eax is free (the hash, already pushed).
__attribute__((naked)) void EndCutterThunk() {
    asm volatile("movl _gc_puppet, %eax\n\t"
                 "testl %eax, %eax\n\t"
                 "je 1f\n\t"
                 "cmpl 0x274(%esi), %eax\n\t"
                 "jne 1f\n\t"
                 "testb $1, _gc_ppWorld\n\t"
                 "jne 1f\n\t"
                 "incl _gc_cutterSkips\n\t"
                 "xorl %eax, %eax\n\t"
                 "ret $0x10\n"
                 "1:\n\t"
                 "jmp *_gc_createObject");
}
// G6, the ram's spherecast filter (imm32 at 0x5F3188; havok_game_filter::isCollisionEnabled calls it cdecl (object,
// data), object never NULL, and stores al: 0x41C074-0x41C081). One cast (0x5F3199) feeds every Rhino and Thrust hit and
// in-charge cutter (0x5F31A9-0x5F3712). Returning false without writing data+4 = neither a hit nor a stop: the puppet's
// charge passes through. Players always (in SP none is hit anyway: MP id 0x57C00A), NPCs (type 0 subtype 2), vehicles
// (type 3) and rfg_movers (type 2 / 1: the cutters; general movers break locally until 7.2) per gc_ppWorld. The wearer
// itself and anything else: the game's filter.
int g_nRam;
bool __cdecl RamFilter(uint8_t* o, uint8_t* d) {
    uint8_t* w = d ? Field<uint8_t*>(d, 0) : nullptr;
    if (o != w && IsPuppet(w)) {
        uint8_t t = Field<uint8_t>(o, 0x7E), s = Field<uint8_t>(o, 0x7F), pol = gc_ppWorld;
        const char* what = t == 0 && s == 3 ? "a player" : t == 0 && s == 2 && !(pol & 2) ? "an NPC"
                         : t == 3 && !(pol & 4) ? "a vehicle" : t == 2 && s == 1 && (!(pol & 1) || (pol & 8)) ? "a building" : nullptr;
        if (what) {
            if (++g_nRam <= 5) Log("%s: G6: its ram passes through %s (#%d)", Who(), what, g_nRam);
            return false;
        }
    }
    return Fn<uint32_t __cdecl(void*, void*)>(site::RhinoFilter)(o, d) & 0xFF;
}

void __cdecl OnSimFrame(); // 0x3EE55C, below

// ---- patch helpers -------------------------------------------------------------------------------------------------
bool Expect(uintptr_t at, const uint8_t* want, size_t n, const char* what) {
    if (!memcmp(Rva(at), want, n)) return true;
    Log("ghost: %s at %X: unexpected bytes", what, (unsigned)at);
    return false;
}
bool ExpectCall(uintptr_t at, uintptr_t fn, const char* what) { // E8 rel32 to the game's own function (no sibling chain)
    uint8_t* s = Rva<uint8_t>(at);
    if (s[0] == 0xE8 && (uintptr_t)(s + 5 + *(int32_t*)(s + 1)) == g_base + fn) return true;
    Log("ghost: %s at %X: not the expected call to %X", what, (unsigned)at, (unsigned)fn);
    return false;
}
void Jmp(uintptr_t at, void* to, size_t len) { // E9 rel32, NOP-padded to len
    uint8_t code[16];
    memset(code, 0x90, sizeof code);
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)to - (g_base + at + 5));
    memcpy(code + 1, &rel, 4);
    WriteCode(Rva(at), code, len);
}

// Checks every site first; writes nothing unless all match. Sim thread (the tick).
bool Install() {
    uint32_t mp = (uint32_t)(g_base + rva::Multiplayer);
    uint8_t air[9] = {0xA0, 0, 0, 0, 0, 0x84, 0xC0, 0x74, 0x41}, melee[9] = {0x80, 0x3D, 0, 0, 0, 0, 0x00, 0x74, 0x2B};
    uint8_t l5m[9] = {0x80, 0x3D, 0, 0, 0, 0, 0x00, 0x74, 0x79}, l5t[9] = {0x80, 0x3D, 0, 0, 0, 0, 0x00, 0x74, 0x32};
    memcpy(air + 1, &mp, 4), memcpy(melee + 2, &mp, 4), memcpy(l5m + 2, &mp, 4), memcpy(l5t + 2, &mp, 4); // ASLR-relocated
    static const uint8_t dmg[8] = {0x83, 0xEC, 0x0C, 0x56, 0x8B, 0x74, 0x24, 0x14}, stats[2] = {0x8B, 0xCE};
    static const uint8_t store[4] = {0x89, 0x6C, 0x24, 0x10};
    static const uint8_t mapAllied[2] = {0x74, 0x08}, mapAlliedFix[2] = {0x74, 0xB6};
    static const uint8_t mapFriendly[9] = {0x87, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00}, mapFriendlyFix = 0x86;
    bool ok = true;
    ok &= ExpectCall(site::SimFrameCall, site::SimFrame, "simulate_frame call");
    ok &= ExpectCall(site::AnimDirectCall, site::AnimDirect, "local anim_direct call");
    ok &= ExpectCall(site::DoFrameCall, site::DoFrame, "player::do_frame call");
    ok &= ExpectCall(site::InteractCall, site::Interact, "object_interaction call");
    ok &= ExpectCall(site::KilledCall, site::Killed, "player_is_killed call");
    ok &= ExpectCall(site::CrouchCall, site::SetSubmode, "PP-CROUCH call");
    ok &= ExpectCall(site::StandCall, site::SetSubmode, "PP-CROUCH stand call");
    ok &= ExpectCall(site::StreamPackCall, site::PackType, "player::stream backpack type call");
    ok &= ExpectCall(site::ShakeStopCall, site::ShakeStop, "G5 backpack deactivate camera_shake_stop call");
    ok &= ExpectCall(site::RippleHumanCall, site::HumanFromHandle, "G4 ripple object_from_handle<human> call");
    ok &= ExpectCall(site::EndCutterCall, site::CreateObject, "G7 backpack deactivate cutter create_object_internal call");
    uint8_t ripple[12] = {0x38, 0x1D, 0, 0, 0, 0, 0x0F, 0x85, 0xB6, 0x03, 0x00, 0x00}, ram[8] = {0xC7, 0x44, 0x24, 0x7C};
    uint32_t ramFilter = (uint32_t)(g_base + site::RhinoFilter), ramMine = (uint32_t)(uintptr_t)&RamFilter;
    memcpy(ripple + 2, &mp, 4), memcpy(ram + 4, &ramFilter, 4); // ASLR-relocated absolutes
    ok &= Expect(site::RippleGate, ripple, 12, "G3 use_ripple Multiplayer test");
    ok &= Expect(site::RamFilterImm, ram, 8, "G6 ram spherecast filter");
    ok &= Expect(site::StatsSkip, stats, 2, "PP-STATS");
    ok &= Expect(site::ListenerStore, store, 4, "listener player store");
    ok &= Expect(site::HumanDamage, dmg, 8, "human_damage prologue");
    ok &= Expect(site::AirSite, air, 9, "PP-AIR");
    ok &= Expect(site::MeleeSite, melee, 9, "PP-MELEE");
    static const uint8_t turn[6] = {0x0F, 0x84, 0x28, 0x01, 0x00, 0x00};
    ok &= Expect(site::MeleeTurn, turn, 6, "melee turn je");
    ok &= Expect(site::MapAllied, mapAllied, 2, "minimap allied npc cast");
    ok &= Expect(site::MapFriendly, mapFriendly, 9, "minimap friendly npc cast");
    if (g_l5) {
        ok &= Expect(site::L5Melee, l5m, 9, "L5 melee");
        ok &= Expect(site::L5Trigger, l5t, 9, "L5 trigger");
    }
    if (!ok) return false;

    gc_mp = g_base + rva::Multiplayer, gc_player = g_base + rva::Player, gc_submode = g_base + site::SetSubmode;
    gc_airMp = g_base + site::AirMp, gc_airBack = g_base + site::AirBack;
    gc_meleeMp = g_base + site::MeleeMp, gc_meleeSp = g_base + site::MeleeSp;
    uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return Log("ghost: VirtualAlloc for the human_damage trampoline failed"), false;
    memcpy(t, dmg, 8); // the 8 prologue bytes (no relative operand), then jmp 0x6CE958
    t[8] = 0xE9;
    int32_t rel = (int32_t)(g_base + site::HumanDamage + 8 - ((uintptr_t)t + 13));
    memcpy(t + 9, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    g_damageNext = (DamageFn)t;

    g_simNext = ChainRel32(site::SimFrameCall, (void*)&OnSimFrame);
    g_animNext = ChainRel32(site::AnimDirectCall, (void*)&OnAnimDirect);
    g_doFrameNext = ChainRel32(site::DoFrameCall, (void*)&OnPlayerDoFrame);
    g_interactNext = ChainRel32(site::InteractCall, (void*)&OnInteract);
    g_killedNext = ChainRel32(site::KilledCall, (void*)&OnKilled);
    ChainRel32(site::CrouchCall, (void*)&CrouchThunk);
    ChainRel32(site::StandCall, (void*)&CrouchThunk);
    gc_packType = ChainRel32(site::StreamPackCall, (void*)&PackTypeThunk);
    gc_shakeStop = ChainRel32(site::ShakeStopCall, (void*)&ShakeStopThunk);
    gc_humanFromHandle = ChainRel32(site::RippleHumanCall, (void*)&RippleHumanThunk);
    gc_createObject = ChainRel32(site::EndCutterCall, (void*)&EndCutterThunk);
    gc_rippleBack = g_base + site::RippleGateBack, gc_rippleMovers = g_base + site::RippleMovers;
    Jmp(site::RippleGate, (void*)&RippleCave, 6);             // the cmp only; the jne at 0x5F18D0 stays
    WriteCode(Rva(site::RamFilterImm + 4), &ramMine, 4);      // RamFilter calls the game's filter itself
    Jmp(site::HumanDamage, (void*)&OnHumanDamage, 8);
    Jmp(site::AirSite, (void*)&AirCave, 5);
    Jmp(site::MeleeSite, (void*)&MeleeCave, 9);
    WriteCode(Rva(site::MapAllied), mapAlliedFix, 2);
    WriteCode(Rva(site::MapFriendly), &mapFriendlyFix, 1);
    Log("puppet: hooks installed (3EE55C 2FC841 6E62D7 6E6B36 6E59E7 630B07 680783 6C1CD4 59AA78, human_damage detour, PP-AIR, PP-MELEE, "
        "minimap 46D3CF 46D43E, pack gates G3 5F18CA G4 5F1942 G6 5F3188 G7 59ABA7)");
    return true;
}

// L5 (operands of 0x6B19C6 / 0x6CC4A1): open while a puppet exists or a session is linked. Idempotent.
void L5(bool open) {
    static bool isOpen;
    if (!g_l5 || open == isOpen) return;
    uint32_t game = (uint32_t)(g_base + rva::Multiplayer), mine = (uint32_t)(uintptr_t)&g_coopOne;
    PatchBytes(site::L5Melee + 2, open ? &game : &mine, open ? &mine : &game, 4, open ? "puppet: L5 melee open" : "puppet: L5 melee closed");
    PatchBytes(site::L5Trigger + 2, open ? &game : &mine, open ? &mine : &game, 4, open ? "puppet: L5 trigger open" : "puppet: L5 trigger closed");
    isOpen = open;
}

// ---- while a puppet exists ------------------------------------------------------------------------------------------
// PP-STATS (non-local tail of player_do_frame reads NULL Mp_game_data), the listener store, the melee turn and the MP
// handle tables (size 1, buckets -> a real zeroed dword: a stray network handle lookup finds nothing instead of dividing
// by zero). L5 (melee / trigger ids advance in SP, for your own Player too, which the capture reads; both guarded blocks
// only bump +0xC4 counters) is L5() above.
// Listener: gamestate_gp_simulate_frame keeps the LAST subtype_objects[3] entry of its per-player loop (0x3EDDC6) and
// reads its +0x2FC stance for the audio listener height (reload 0x3EE16B, read 0x3EE47A); the ghost is appended last.
// NOPed, [esp+0x10] keeps entry 0 (0x3EDB88), which is you (Create refuses index 0). One player: same value either way.
bool Lifetime(bool on) {
    static const uint8_t mov[2] = {0x8B, 0xCE}, jmp[2] = {0xEB, 0x5E};
    static const uint8_t store[4] = {0x89, 0x6C, 0x24, 0x10}, nop[4] = {0x90, 0x90, 0x90, 0x90};
    // pp_stats=0 (E2 control run) leaves 0x6E6B44 alone both ways: the first puppet frame must then crash (Puppet_Install)
    if (g_ppStats && !PatchBytes(site::StatsSkip, on ? mov : jmp, on ? jmp : mov, 2, on ? "puppet: PP-STATS on" : "puppet: PP-STATS off") && on)
        return false;
    PatchBytes(site::ListenerStore, on ? store : nop, on ? nop : store, 4, on ? "puppet: listener store off" : "puppet: listener store restored");
    static const uint8_t turn[6] = {0x0F, 0x84, 0x28, 0x01, 0x00, 0x00}, nop6[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    PatchBytes(site::MeleeTurn, on ? turn : nop6, on ? nop6 : turn, 6, on ? "puppet: melee turn uses its own heading" : "puppet: melee turn restored");
    for (Table& t : g_tables) { // .data: plain writes
        uint32_t* b = Rva<uint32_t>(t.at + 0x40);
        if (on && !t.primed) {
            if (b[1]) { Log("puppet: hash table %X already has size %u (MP was initialised), left alone", (unsigned)t.at, b[1]); continue; }
            t.buckets = b[0], t.size = b[1], t.primed = true;
            b[0] = (uint32_t)(uintptr_t)&g_zeroBucket, b[1] = 1;
            Log("puppet: hash table %X primed (size 1, empty bucket)", (unsigned)t.at);
        } else if (!on && t.primed) {
            if (b[0] == (uint32_t)(uintptr_t)&g_zeroBucket && b[1] == 1) b[0] = t.buckets, b[1] = t.size;
            t.primed = false;
            Log("puppet: hash table %X restored", (unsigned)t.at);
        }
    }
    // setMaterial resolves texture handles at every draw (get_dev_texture 0xF1DB0: Bm_bitmaps[h]+4, its +0x28), so the
    // placeholder's +4 pointed at the right peg entry fixes every instance; one aligned dword each way for the render
    // thread. Only while a puppet exists: an items peg unload (DLC init) would reset the good handle, not this one.
    uint8_t* bm = *Rva<uint8_t*>(site::BmBitmaps);
    auto load = Fn<int __cdecl(const char*, bool)>(site::BmLoad);
    int n = 0;
    for (Alias& a : g_alias) {
        if (on && a.h < 0) {
            int hb = load(a.bad, false), hg = load(a.good, false);
            if (hb < 0 || hg < 0) continue;
            void*& pb = Field<void*>(bm, 12 * hb + 4);
            void* pg = Field<void*>(bm, 12 * hg + 4);
            // only a real placeholder; the target single-frame (bm_get_entry 0x163C3A animates from the index otherwise)
            if (pb != *Rva<void*>(site::BogusPegEntry) || !pg || pg == pb || Field<uint16_t>(pg, 0x10) > 1) continue;
            a.h = hb, a.was = pb, pb = pg, n++;
        } else if (!on && a.h >= 0)
            Field<void*>(bm, 12 * a.h + 4) = a.was, a.h = -1, n++;
    }
    if (n) Log("puppet: %d hammer low-res textures %s", n, on ? "aliased (sledge_up_N_low_d/_s -> _d_low/_s_low)" : "restored");
    return true;
}

void Detach() { // the puppet is gone or about to go: everything back to vanilla, the ring forgotten
    gc_puppet = nullptr, g_handle = ~0u;
    Lifetime(false);
    L5(g_net);
    g_count = g_head = g_sent = 0, g_seeded = false, g_visible = g_sentVisible = true, g_lastTime = 0, g_info = (void*)1, g_slot = -1;
    g_pack = 0, g_bpResync = true; // a new puppet starts with none (PackTypeThunk); the partner's active pack resyncs
    g_driving = false, g_last = {}, g_capPlayer = nullptr; // nothing from the previous level reaches the next puppet
}

// Re-resolves the handle every frame; drops the puppet when the game destroyed it (player_level_shutdown_all at unload).
uint8_t* Puppet() {
    if (!gc_puppet) return nullptr;
    uint8_t* o = ObjectFromHandle(g_handle);
    if (o == gc_puppet && o != LocalPlayer() && Field<int8_t>(o, 0x7E) == 0 && Field<int8_t>(o, 0x7F) == 3 &&
        !(Field<uint8_t>(o, 0x54) & 0x10))
        return o;
    Log("%s: puppet %p (handle %08X) is gone (level unload?); recreated when the level is ready%s", Who(), (void*)gc_puppet,
        g_handle, g_isPartner ? " and the partner shows" : " and you are on foot");
    Detach();
    return nullptr;
}

int ListIndex(const uint8_t* p) { // position in World.subtype_objects[3] (+0x3F4 uint16 indices into +0x64, +0x3FC count)
    uint8_t* w = Rva<uint8_t>(rva::World);
    uint16_t* idx = Field<uint16_t*>(w, 0x3F4);
    uint8_t** all = Field<uint8_t**>(w, 0x64);
    for (int i = 0, n = Field<int>(w, 0x3FC); i < n; i++)
        if (all[idx[i]] == p) return i;
    return -1;
}

bool Ready(const uint8_t* me) { // as player_process_all: the character instance is streamed in
    uint8_t* ci = Field<uint8_t*>(me, 0x32C);
    return ci && Field<uint8_t>(ci, 0x34) && Field<void*>(me, 0xCC); // rdd.m_loaded (bool), as player_process_all 0x6E7366
}

// The WC/MP recipe (process_create_packet 0x5E0657-0x5E07A3) with your own h_info, so the model is resident. The kind
// (ghost / partner) follows g_net.
bool Create(uint8_t* me, vector pos, matrix orient) {
    g_isPartner = g_net;
    if (!Lifetime(true)) return Log("%s: PP-STATS not in place, puppet not created", Who()), false;
    alignas(4) uint8_t props[0x10] = {};
    Fn<void* __thiscall(void*, void*, vector*, matrix*, char*)>(site::HumanProps)(props, Field<void*>(me, 0xCC), &pos, &orient, nullptr);
    unsigned hash = Fn<unsigned __cdecl()>(site::ClassHash)();
    uint8_t* p = Fn<uint8_t* __thiscall(void*, unsigned, void*, void*, unsigned)>(site::CreateObject)(Rva(rva::World), hash, props, nullptr, ~0u);
    if (p) {
        auto setState = Fn<void __cdecl(void*, int, int, float)>(site::SetState);
        setState(p, 0, 0, -1.0f); // HS_STANDARD, HMS_IDLE
        Field<void(__thiscall**)(void*, vector*, matrix*, bool)>(p, 0)[0x30 / 4](p, &pos, &orient, true); // update_pos_and_orient
        setState(p, 0, 0, -1.0f);
        Fn<void __cdecl(void*, int, bool)>(site::SetZoom)(p, 0, false);
        Fn<void __cdecl(void*, bool)>(site::SetVisible)(p, true);
    }
    Fn<void __thiscall(void*)>(site::PropsDtor)(props);
    if (!p) return Log("%s: world::create_object_internal returned NULL", Who()), Lifetime(false), false;
    // Ally / escort squad handles: player::player runs once per pool slot (0x6DE961 skips these two) and the pool never
    // clears a reused slot, so they may hold an old Player's squads; teardown (player_level_shutdown -> free_this, SP
    // branch 0x6CD02C) would disband and destroy those live squads. Every consumer tests for -1.
    Field<uint32_t>(p, 0x24C4) = ~0u, Field<uint32_t>(p, 0x24C8) = ~0u;
    Field<uint32_t(__thiscall**)(void*, int)>(p, 0)[0x88 / 4](p, 0); // human::set_team(HUMAN_TEAM_GUERILLA)
    Field<uint8_t>(p, 0x54) |= 0x20;   // object_flags.no_save: saves skip it, the tick recreates it after a load
    Field<uint32_t>(p, 0xDCC) |= 0x10; // player_flags.never_die (never 0x20, which mission cleanup clears)
    Fn<void __thiscall(void*)>(site::SimReset)(p + 0x1274); // SP never resets mp_sim_state (player::serialize: MP only)
    Field<uint32_t>(p, 0x1268) = 0;   // mp_last_respawn_time: process_state applies only states newer than this + 500
    int idx = ListIndex(p), n = Field<int>(Rva(rva::World), 0x3FC);
    Log("%s: created puppet %p handle %08X at %.1f,%.1f,%.1f; Player %p; players %d; puppet list index %d; mp_data "
        "+0x126C = %08X (expect FFFFFFFF); team %d",
        Who(), (void*)p, Field<unsigned>(p, 0x6C), pos.x, pos.y, pos.z, (void*)me, n, idx, Field<unsigned>(p, 0x126C),
        Field<int>(p, 0xC98));
    if (idx < 1) { // entry 0 must be you: the listener height reads its stance (0x3EDB88, store at 0x3EDDC6 NOPed)
        Log("%s: puppet at list index %d, destroyed again", Who(), idx);
        Fn<void __cdecl(void*)>(site::Shutdown)(p);
        Lifetime(false);
        return false;
    }
    gc_puppet = p, g_handle = Field<unsigned>(p, 0x6C);
    L5(true);
    return true;
}

// The puppet's pack, made as player::do_frame makes yours (0x6D56C3-0x6D56D2); id -1 = none. event_player_picked_up drops
// the one worn first (0x5F2409-0x5F243B: event_player_dropped(1) = detach, +0x1170 = 0, destroy) and wears the new one
// (+0x1170, +0x274, human_wear_prop). Visual only: your +0x1170, s_lastBackpackIndex and HUD are never touched (pickup HUD
// is for the local player only, 0x5F249C); only PackEffects activates it. picked_up attaches only a streamed pack
// (0x5F23D3): one it refused is destroyed, never left lying in the world as a pickup, and the old one stays worn (the
// drop comes after that test). Returns false only for that refusal, so Effects retries; a create 0 is final (none).
bool SetPack(uint8_t* p, int id) {
    static int s_refused = -2; // the id last refused: logged once, as Effects retries it per ring entry
    uint8_t* bp = id < 0 ? nullptr : Fn<uint8_t* __cdecl(int, void*, void*)>(site::PackCreate)(id, p + 0x04, p + 0x10);
    if (bp) Fn<void __thiscall(void*, void*, bool, bool)>(site::PackPickedUp)(bp, p, true, false);
    else if (uint8_t* old = Field<uint8_t*>(p, 0x1170)) Fn<void __thiscall(void*, bool)>(site::PackDropped)(old, true);
    bool loose = bp && Field<uint8_t*>(p, 0x1170) != bp;
    if (loose) Fn<bool __thiscall(void*, void*, bool)>(site::DestroyObject)(Rva(rva::World), bp, false);
    if (!loose || id != s_refused)
        Log("%s: puppet backpack -> id %d%s", Who(), id, id >= 0 && !bp ? " FAILED (create 0): none" : loose ? " FAILED (not attached), retrying" : "");
    s_refused = loose ? id : -2;
    return !loose;
}

void Destroy() {
    uint8_t* p = Puppet();
    if (!p) return;
    // its pack first, as MP does for a leaving player (remove_player_common 0x5BA14B-0x5BA17D). Quiet, as a hidden end:
    // a session-down drop of the partner's live Rhino / Thrust cuts no end hole the partner never made (G7 counts it).
    uint8_t world = gc_ppWorld;
    gc_ppWorld &= ~1;
    SetPack(p, -1);
    gc_ppWorld = world;
    Detach();
    Fn<void __cdecl(void*)>(site::Shutdown)(p); // the game's own per-player teardown (MP remove_player uses it too)
    Log("%s: puppet destroyed, %d players left", Who(), Field<int>(Rva(rva::World), 0x3FC));
}

// ---- ring of captured snapshots --------------------------------------------------------------------------------------
constexpr int kRing = sizeof g_ring / sizeof *g_ring;
void Push(const Snap& s) {
    if (g_count == kRing) g_head = (g_head + 1) % kRing, g_count--, g_sent -= g_sent > 0; // full: drop the oldest
    g_ring[(g_head + g_count++) % kRing] = s;
}
void PushGhost(Snap s) { // arrival 300 ms after the capture, network_time = arrival + interpolation delay
    s.due = s.at + kDelayMs, s.st.time = s.at + kDelayMs + kInterpMs;
    Push(s);
}

// A stop state: no velocity or stick, no sprint, no detonation, trigger released (up id = down id). While you drive
// (hidden ghost / PSTATE flag 0), while the game is paused (PSTATE), and for a partner puppet that must stand hidden.
void Still(MpState& st) {
    st.vel = {}, st.forward = st.sideways = 0;
    st.bits = (st.bits & ~(8u | 0x2800u | 0xC000000u)) | ((st.bits >> 24 & 3) << 26);
}

// Trigger ids (apply_weapon_state 0x5824C3-0x58268E): a SINGLE weapon fires once per down-id change; an AUTOMATIC one
// keeps firing while the state's up id != down id and releases when they match. SINGLE/CHARGE: L5 bumps Player+0xC4
// down id once per shot (0x6CC4AA), so count those; AUTOMATIC never bumps it in SP (0x6BDFF9 is MP-gated): count
// presses of the trigger level instead and hold up = down - 1 while it is down. On a turret get_weapon / is_trigger_pulled
// read the turret's gun (0x65A126) while the ghost holds nothing (manning unequips, +0xA30 = 0 at 0x653846): turret shots
// are not counted, only re-baselined, so nothing fires once you are off it.
void TriggerIds(uint8_t* me, uint32_t c4, int& down, int& up) {
    bool pulled = Fn<uint32_t __cdecl(void*)>(site::TriggerPulled)(me) & 0xFF, automatic = false;
    int c4down = (c4 >> 14) & 3;
    if (!Fn<void* __cdecl(void*)>(site::MannedTurret)(me)) {
        uint8_t* w = Fn<uint8_t* __cdecl(void*)>(site::GetWeapon)(me);
        uint8_t* wi = w ? Field<uint8_t*>(w, 0x148) : nullptr;
        automatic = wi && Field<int>(wi, 0x104) == 1; // weapon_info.trigger_type
        if (automatic || !g_l5) g_down += pulled && !g_prevPulled;
        else g_down += (c4down - g_prevC4Down) & 3;
    }
    g_prevPulled = pulled, g_prevC4Down = c4down;
    down = g_down & 3;
    up = automatic && pulled ? (down + 3) & 3 : down;
}

void Out(const Snap& s, const vector& where) { g_out = s, g_outWhere = where, g_outFresh = true; } // for Puppet_Out

// After the simulation: the local Player as player_multi_registration::get_state 0x5F49C0 builds it, except the aim
// (Rfg_camera real_orient) and the stick (OnAnimDirect), which SP never stores in +0x1238 / +0x117C.
void Capture(uint8_t* me) {
    uint32_t now = ClockNow();
    if (me != g_capPlayer) { // a new level's Player: no stop state, trigger baseline or PSTATE from the old one
        g_capPlayer = me, g_last = {}, g_out = {}, g_outFresh = false;
        g_prevC4Down = Field<uint32_t>(me, 0xC4) >> 14 & 3, g_prevPulled = false;
    }
    if (unsigned veh = Field<unsigned>(me, 0xAC4); veh != ~0u) { // in a vehicle: hidden
        Snap s = g_last;
        s.bp = uint8_t((s.bp & 0xF0) | (g_bpCount & 0xF)); // a reset g_last would send counter 0: a false edge on exit
        if (!s.at) s.st.pos = Field<vector>(me, 0x04), s.st.orient = Field<matrix>(me, 0x10); // never on foot this level
        Still(s.st);
        s.at = now, s.visible = false;
        if (g_net) { // every frame: the receiver hides on the flag and F7 needs where you drive
            uint8_t* v = ObjectFromHandle(veh);
            Out(s, Field<vector>(v ? v : me, 0x04));
        } else if (!g_driving && g_last.at)
            PushGhost(s); // one stop state, then the ghost hides
        g_driving = true;
        g_prevC4Down = Field<uint32_t>(me, 0xC4) >> 14 & 3, g_prevPulled = false; // seat shots (L5) are not replayed
        return;
    }
    g_driving = false;
    Snap s{};
    MpState& st = s.st;
    st.pos = Field<vector>(me, 0x04);
    st.orient = Field<matrix>(me, 0x10);
    st.vel = Field<vector>(me, 0x270); // actual_velocity
    st.ragdoll = Field<int>(me, 0x33C);
    if (st.ragdoll == 1) { // HUMAN_RAGDOLL_STATE_ACTIVE: the body, not the object (get_state's jump table, 0x5F4B9B)
        Fn<void __cdecl(int, vector*, int)>(site::RagdollPos)(Field<int>(me, 0x344), &st.pos, 1);
        Fn<void __cdecl(int, vector*)>(site::RagdollVel)(Field<int>(me, 0x344), &st.vel);
    }
    if (!g_net) st.pos.x += g_offset;
    st.camOrient = Field<matrix>(Rva(site::RfgCamera), 0x50);
    if (g_moveSeen) st.forward = g_fwd, st.sideways = g_side; // 0 when normal movement did not run (ladder, cover)
    g_moveSeen = false;
    int maxHp = Field<int>(me, 0x910);
    st.hpPct = maxHp ? Field<float>(me, 0x914) / maxHp : 1.0f;
    st.ladder = Field<unsigned>(me, 0xB18);
    st.knockdown = Fn<int __thiscall(void*)>(site::TimeUntil)(me + 0x920);
    st.turretNet = st.vehNet = 0xFFFF, st.turretGame = st.vehGame = ~0u; // none, and the cache dwords set in every state
    uint32_t c4 = Field<uint32_t>(me, 0xC4), b4 = Field<uint32_t>(me, 0xB4), b8 = Field<uint32_t>(me, 0xB8);
    // +0xC4 bits 23/24 (in-air / detonated, set at 0x301913 / 0x301089) are cleared only by MP code (0x551276, 0x5DD6F1,
    // 0x6E69B3 non-local): clear them here so the capture sees one edge per detonation.
    if (c4 & 0x1800000) Field<uint32_t>(me, 0xC4) = c4 & ~0x1800000u;
    uint32_t pf = Field<uint32_t>(me, 0xDCC);
    // +0x239C (mp_ladder_state) is written only under Multiplayer (0x6123B0, 0x6B8945, 0x6B8D0C, 0x6C484B, 0x6C4F18):
    // frozen in SP. Send what MP's sender writes during a normal climb (0x6B894E, 0x6C513C): mode 1 (+0xA2, track the
    // rung, compute_ladder_stick_value 0x55F983) with your current rung (+0xA3), as 0x55F946 computes it.
    if (b8 & 0x8000)
        st.ladderState = uint16_t(1 | (Fn<int __cdecl(void*, unsigned)>(site::LadderRung)(me, st.ladder) & 0xFF) << 8);
    uint8_t* item = Field<uint8_t*>(me, 0xA30); // desired_equipped_inv_item
    int slot = item ? Field<int>(item, 0x10) : 7, down, up;
    TriggerIds(me, c4, down, up);
    st.bits = (slot & 7) | (pf >> 6 & 1) << 3 | (c4 >> 9 & 3) << 4 | (b8 >> 15 & 1) << 6 |
              (Fn<uint32_t __cdecl(void*, bool)>(site::IsReloading)(me, false) & 1) << 7 | (pf >> 8 & 1) << 8 |
              (Fn<uint32_t __cdecl(void*)>(site::IsZoomed)(me) & 1) << 9 | (b4 >> 24 & 1) << 10 | (c4 >> 23 & 1) << 11 |
              (c4 >> 22 & 1) << 12 | (c4 >> 24 & 1) << 13 | (c4 >> 1 & 0xFF) << 16 | (uint32_t)down << 24 |
              (uint32_t)up << 26 | (c4 >> 18 & 3) << 28 | (Fn<uint32_t __cdecl(void*)>(site::IsCrouching)(me) & 1) << 30;
    s.at = now, s.info = item ? Field<void*>(item, 0x08) : nullptr, s.visible = true;
    uint8_t* bp = Field<uint8_t*>(me, 0x1170), *bi = bp ? Field<uint8_t*>(bp, 0x270) : nullptr; // worn pack, its info
    s.pack = bi ? uint8_t(Field<int>(bi, 0x50) + 1) : 0;                                       // id (0x5AFDA3)
    // Activity (protocol 6). The counter steps when activated_time's end tick (+0x268) changes to a value other than -1
    // on the same pack: only activate writes one (0x5F2337); ctor, pool init, reset and deactivate write -1. So a
    // Shockwave one-shot, whose +0x254 is 0 again by now, counts too. Another pack object re-baselines, uncounted.
    uint32_t tick = bp ? Field<uint32_t>(bp, 0x268) : ~0u;
    g_bpCount += bp == g_bpObj && tick != g_bpTick && tick != ~0u;
    g_bpObj = bp, g_bpTick = tick;
    s.bp = uint8_t((g_bpCount & 0xF) | (bp ? (Field<uint8_t>(bp, 0x254) != 0) << 4 | (Field<uint8_t>(bp, 0x2AC) != 0) << 5 |
                                                 (Field<uint8_t>(bp, 0x2AD) != 0) << 6 : 0));
    s.fuel = bp ? uint8_t(fminf(fmaxf(Field<float>(bp, 0x25C), 0.0f), 1.0f) * 255.0f + 0.5f) : 0; // NaN -> 0
    g_last = s;
    if (!g_net) return PushGhost(s);
    g_detCharges += (st.bits & 0x2000) != 0, g_detAir += (st.bits & 0x800) != 0;
    Out(s, Field<vector>(me, 0x04));
}

// Weapon switch and zoom are not in process_state; MP does them per state in apply_compressed_state (0x5F95A2).
// false = player_equip_item refused: it returns 0 only while the puppet is HS_LADDER (+0x2F4 == 5, 0x6C2A23-0x6C2A2E);
// already equipped and NULL (unarmed) return 1. The caller retries on the next state, as MP re-equips every packet; the
// retry finds the item already in the inventory.
bool GiveWeapon(uint8_t* p, void* info, int slot) {
    uint8_t* item = nullptr;
    if (info) {
        uint8_t* head = Field<uint8_t*>(p, 0xA2C); // inventory: circular list, +0x08 inv_item_info*
        for (uint8_t* it = head; it && !item;) {
            if (Field<void*>(it, 0x08) == info) item = it;
            if ((it = Field<uint8_t*>(it, 0)) == head) break;
        }
        if (!item) {
            if (uint8_t* old = Fn<uint8_t* __cdecl(void*, int)>(site::ItemInSlot)(p, slot))
                Fn<void __cdecl(void*, void*, bool, bool)>(site::DeleteItem)(p, old, false, false);
            item = Fn<uint8_t* __cdecl(void*, void*, int, int, int, bool, bool, bool)>(site::AddItem)(p, info, 1, 99999, slot, false, false, false);
        }
    }
    if (!(Fn<uint32_t __cdecl(void*, void*, void*, bool)>(site::EquipItem)(p, item, nullptr, true) & 0xFF)) // NULL = unarmed, as MP
        return false;
    Log("%s: puppet weapon -> slot %d item %p", Who(), slot, (void*)item);
    return true;
}

// Arrival (ghost: capture + 300 ms; partner: when the PSTATE is drained): the state goes into the sim buffer with its
// network_time (st.time, set by PushGhost / Puppet_In), kept monotonic here.
void Deliver(uint8_t* p, Snap& s) {
    MpState& st = s.st;
    uint32_t& c4 = Field<uint32_t>(p, 0xC4);
    // Back from a drive, from outside your zones or from a partner load: no interpolation across it. Here, not in
    // Effects: later it would wipe the states queued since. The ids moved meanwhile (a hidden state is Hidden(), the
    // last visible one's ids; a load means a new Player), so re-seed them too; the reset emptied the buffer, so no
    // stale-id state runs against the new seed.
    bool back = s.visible && !g_sentVisible;
    if (back) Fn<void __thiscall(void*)>(site::SimReset)(p + 0x1274);
    g_sentVisible = s.visible;
    if (!g_seeded || back) { // start from the sender's ids: no spurious first jump, swing, shot or throw (BeforeSim
        // applies nothing until Clock reaches this state's network_time)
        c4 = (c4 & ~0xFC7FEu) | (st.bits >> 15 & 0x1FE) | (st.bits >> 4 & 3) << 9 | (st.bits >> 24 & 3) << 14 |
             (st.bits >> 26 & 3) << 16 | (st.bits >> 28 & 3) << 18;
        g_seeded = true;
    }
    if (st.time < g_lastTime) st.time = g_lastTime; // monotonic, and inside add_state's 5 s window (~Clock + 100)
    // ponytail: thin to one state per 5 ms, so the 25-state buffer (add_state 0x568B8C) spans >= 120 ms > kInterpMs at
    // any fps and BeforeSim's gate always finds a state <= Clock. Nothing is lost: the ids are cumulative counters the
    // next state carries, and visibility / weapon / zoom / detonation run per ring entry in Effects. Never the hidden
    // stop state: without it the hidden ghost would extrapolate your last move and keep a held trigger firing.
    if (s.visible && Field<uint32_t>(p + 0x1274, 0) && st.time < g_lastTime + 5) return;
    g_lastTime = st.time;
    Fn<void __thiscall(void*, MpState*)>(site::AddState)(p + 0x1274, &st);
    g_delivered++;
}

// The partner's pack activity (backpack_sync.md §4.1), from Effects after SetVisible and SetPack. As an MP client does for
// a remote player (toggle_activation 0x5F8271 / 0x5F81EA): the full activate / deactivate on the puppet's own pack, so
// every effect, sound, material and ability built for a remote wearer plays here, and the local-only parts (shakes,
// xray, HUD, FOV, cover, zoom) exclude the puppet by themselves. Never the SP toggle or ramp: can_activate could refuse.
// The ghost's pack stays as ver6.2 (its Snap carries the sender's counter, not an edge).
void PackEffects(uint8_t* p, const Snap& s) {
    uint8_t* bp = Field<uint8_t*>(p, 0x1170), *bi = bp ? Field<uint8_t*>(bp, 0x270) : nullptr;
    if (!g_isPartner || !bi || Field<uint8_t*>(bp, 0x274) != p) return;
    int id = g_pack - 1; // the worn pack's (SetPack records it); == s.pack - 1 past the next test
    bool on = Field<uint8_t>(bp, 0x254), level = s.bp & kLevel;
    // Unguarded in the game: only for an active pack. quiet: our own end (hidden, partner paused, away) while the partner's
    // Rhino or Thrust goes on: the world bit off for this call, so G7 cuts no end hole there (gc_cutterSkips counts it);
    // the partner's level 0 cuts the one real end hole. Nothing else reads gc_ppWorld inside deactivate.
    auto deactivate = [&](const char* why, bool quiet = false) {
        uint8_t world = gc_ppWorld;
        if (quiet) gc_ppWorld &= ~1;
        Fn<void __thiscall(void*)>(site::PackDeactivate)(bp);
        gc_ppWorld = world;
        Log("%s: puppet pack off id %d%s", Who(), id, why);
    };
    // Hidden, unstreamed, or the partner's new pack not worn yet (SetPack waits): no quake or heal runs at a puppet you
    // do not see; the activation glares and sound pass 0x57C8BF only while it is visible, so the unhide re-activates.
    if (!s.visible || !(Field<uint8_t>(p, 0x55) & 0x10) || g_pack != s.pack) {
        if (on) deactivate(" (hidden)", true);
        g_bpResync = true;
        return;
    }
    float fuel = s.fuel / 255.0f;
    if (Field<uint8_t>(bi, 0x248)) { // Jetpack: flight rides state bit 8; burn the partner's tank, not a simulated one
        // Frozen too: Still keeps bit 8, so a paused partner's puppet hovers, and the paused tank is not stale.
        Field<float>(bp, 0x25C) = Field<float>(p, 0xDBC) = fuel; // flight burns +0xDBC (0x69E45B); idle refill reads +0x25C
        bool flying = s.st.bits & 0x100;
        if (flying && !g_jetLogged) Log("%s: jetpack fuel written (%.2f)", Who(), fuel);
        g_jetLogged = flying;
        return;
    }
    if (s.bp & kFrozen) { // the partner paused: its stale level would hold the pack on with no end; the resume resyncs
        if (on) deactivate(" (partner paused)", true);
        g_bpResync = true;
        return;
    }
    if (!Field<uint8_t>(bi, 0x6EE) || id < 0 || !(kPackAllow >> id & 1)) return; // not activatable, or not in this phase
    // On a counter step, or a resync of a pack the partner has on (kStale: a step from while you were away or behind a
    // loss burst). Never on a level alone: the puppet's own fuel-out (0x5F3B12) may end it one state before the partner's
    // level 0. A resync never re-launches a Thrust mid-flight (info+0x198: active jump_boost). An edge whose level is
    // already 0 is stale: every deactivatable pack is still on at the capture that counts it, and that capture goes out
    // at once (Puppet_Step), even a low Thrust's, and the next state repeats its level 1 (Puppet_Out): if that capture's
    // PSTATE is lost, the step arrives with level 1 about 50 ms late, and the level 0 after it (kLanded) ends the Thrust
    // with its dust and end hole. One-shots (no info+0x6EF) have no level. Such a late Thrust step with kLanded (only the
    // repeat has it: activate clears +0x2AD) is dropped if the puppet is on the ground: activate would jump it (0x5F22AC).
    if (((s.bp & kEdge) && (level || !Field<uint8_t>(bi, 0x6EF)) &&
         !(Field<uint8_t>(bi, 0x198) && (s.bp & kLanded) && !(Fn<uint32_t __cdecl(void*)>(site::HumanInAir)(p) & 0xFF))) ||
        ((g_bpResync || (s.bp & kStale)) && level && !on && !Field<uint8_t>(bi, 0x198))) {
        if (on) Fn<void __thiscall(void*)>(site::PackDeactivate)(bp);
        Field<uint8_t>(bp, 0x2AC) = (s.bp & kUp) != 0; // Thrust direction, read by activate (0x5F22C6)
        Fn<void __thiscall(void*)>(site::PackActivate)(bp);
        Field<float>(bp, 0x25C) = fuel;                // after: activate subtracts the activation cost (0x5F20A5)
        // R4, Thrust: activate jumps a puppet on the ground (human_jump 0x5F22AC) and the partner's launch is also in this
        // state's jump id, which apply_movement_state replays while it differs from +0xC4 bits 9-10 (0x5CF418-0x5CF433).
        // human_jump bumps that id only when it jumps (in the air 0x6C731D, 1.1 s cooldown 0x6C7379, state 0x6C74E1
        // return before 0x6C75DA), so where the two machines disagree the puppet would launch again later, up to 3 times
        // (+1 per jump, 2 bits). This state's id is the puppet's: one launch, plus the boost (apply_jump_boost).
        uint32_t& c4 = Field<uint32_t>(p, 0xC4), jump = (s.st.bits & 0x30) << 5;
        if (Field<uint8_t>(bi, 0x198) && (c4 & 0x600) != jump) {
            Log("%s: Thrust: puppet jump id %u -> %u (the state's): no second launch", Who(), (c4 & 0x600) >> 9, jump >> 9);
            c4 = (c4 & ~0x600u) | jump;
        }
        Log("%s: puppet pack on id %d%s, visible bit %d", Who(), id, s.bp & kEdge ? "" : " (resync)",
            Field<uint8_t>(p, 0x56) >> 7); // the effects_alr_attack gate (0x57C8BF): expect 1
    } else if (!level && on) {
        bool q = s.bp & (kAway | kStale); // an end first seen after your menu (or a late step): ours, at an old place
        Field<uint8_t>(bp, 0x2AD) = !q && (s.bp & kLanded); // Thrust landing dust (0x59ABBA)
        deactivate(q ? " (away)" : "", q);
    } else
        Field<float>(bp, 0x25C) = fuel; // the glow; the local drain between states ends it within one state of the partner
    g_bpResync = false;
}

// When Clock reaches the state's network_time (ghost: capture + 400 ms), i.e. when process_state applies it: obtain_state
// (0x5E1CBC-0x5E1CDF) takes the newest state with time <= Clock (BeforeSim's gate guarantees one) and copies its
// discrete bits (0x563B9B). Visibility, weapon, zoom and detonation therefore land with the replayed movement and ids,
// not kInterpMs ahead of them.
void Effects(uint8_t* p, Snap& s) {
    uint32_t bits = s.st.bits;
    if (s.visible != g_visible) Fn<void __cdecl(void*, bool)>(site::SetVisible)(p, s.visible), g_visible = s.visible;
    int slot = bits & 7;
    if ((s.info != g_info || slot != g_slot) && GiveWeapon(p, s.info, slot)) g_info = s.info, g_slot = slot;
    // The partner's pack, once per change, recorded only once worn. Hidden or unstreamed (+0x55 bit 0x10, set by
    // create_object_from_stub 0x53E479 only inside the 340 m stream cylinder) waits: a pack made at an unstreamed
    // puppet's position is unstreamed too, and picked_up refuses it (0x5F23D3). Zones load to ~510 m, so that happens.
    if (s.visible && s.pack != g_pack && (Field<uint8_t>(p, 0x55) & 0x10) && SetPack(p, s.pack - 1)) g_pack = s.pack, g_bpResync = true;
    PackEffects(p, s); // after SetVisible and SetPack (backpack_sync.md §4.1)
    bool zoomed = Fn<uint32_t __cdecl(void*)>(site::IsZoomed)(p) & 0xFF;
    if ((bits & 0x200) && !zoomed) Fn<void __cdecl(void*, int, bool)>(site::SetZoom)(p, 2, false); // PZS_FINE_AIM
    else if (!(bits & 0x200) && zoomed) Fn<void __cdecl(void*, int, bool)>(site::SetZoom)(p, 0, false);
    // MP detonates remote charges from read_control_flags (0x5DD6AF / 0x5DD713), not from the state: do it here.
    if (bits & 0x2000) Fn<uint32_t __cdecl(unsigned)>(site::DetonateCharges)(g_handle);
    if (bits & 0x800) Fn<uint32_t __cdecl(unsigned)>(site::DetonateInAir)(g_handle);
}

// ---- 0x3EE55C: gameplay frames only, silent while s_pauseGame --------------------------------------------------------
void BeforeSim() {
    uint8_t* p = Puppet();
    if (!p || Mp() || g_isPartner != g_net) return; // a leftover of the other kind waits for Puppet_Tick to remove it
    uint32_t now = ClockNow();
    // entries are in ring order with due <= time: the effects loop only pops entries the first loop delivered
    while (g_sent < g_count && (int32_t)(now - g_ring[(g_head + g_sent) % kRing].due) >= 0)
        Deliver(p, g_ring[(g_head + g_sent) % kRing]), g_sent++;
    while (g_sent && (int32_t)(now - g_ring[g_head].st.time) >= 0)
        Effects(p, g_ring[g_head]), g_head = (g_head + 1) % kRing, g_count--, g_sent--;
    // Only once the oldest buffered state is <= Clock: with none, obtain_state (0x568C5F) copies the NEWEST (a future
    // state, ~Clock+100) and the replay would run 100 ms ahead, then step back and re-fire ids (after creation, after
    // every vehicle exit). Skipping is the same as an empty buffer (0x5E1CE6), except the +0xDD0 bit 3/7 latches, which
    // only apply_weapon_state sets (0x58264D / 0x582705), stay for the next apply. Sim state: count +0, states +4.
    uint8_t* ss = p + 0x1274;
    if (!Field<uint32_t>(ss, 0) || Field<uint32_t>(ss, 4 + 0x48) > now) return;
    g_simAt = now;
    Fn<void __cdecl(void*)>(site::ProcessState)(p);
    if (!g_l5) { // fallback: the applied state's melee and trigger-down ids, so swings and shots do not re-fire
        MpState st;
        bool extrapolated;
        if (Fn<uint32_t __thiscall(void*, unsigned, MpState*, bool*)>(site::ObtainState)(p + 0x1274, now, &st, &extrapolated) & 0xFF)
            Field<uint32_t>(p, 0xC4) = (Field<uint32_t>(p, 0xC4) & ~0xC1FEu) | (st.bits >> 15 & 0x1FE) | (st.bits >> 24 & 3) << 14;
    }
}

// The partner on the minimap at its real position (its vehicle while it drives, also outside your streamed zones and
// with no puppet), as the activities draw theirs: a temporary icon re-added every gameplay frame (site::AddTempIcon).
// After the simulation, so cartographer::process (inside it) has already decided whether adds are allowed this frame.
void Blip() {
    vector at;
    if (!g_icon || !Puppet_PartnerPos(&at.x)) return;
    Fn<void __thiscall(void*, vector*, int, int, unsigned, void*, int, unsigned)>(site::AddTempIcon)(
        Rva(site::Cartographer), &at, site::StarGreen, 1, ~0u, Rva(site::DefaultMapColor), 0, ~0u); // stick 1, as squads (0x39AEE2)
}

void __cdecl OnSimFrame() {
    if (g_on || g_net) BeforeSim();
    ((void(__cdecl*)())g_simNext)();
    if (uint8_t* me = LocalPlayer()) {
        if ((g_on && gc_puppet) || g_net) Capture(me);
        if (g_net) Blip();
    }
}

// Why no puppet may be created now (NULL = it may). F6 shows it; the re-create after a level load waits it out (in WC
// wrecking_crew_process would hide and teleport it as a contestant; a ghost made in a vehicle would never hide).
const char* Refused(uint8_t* me, bool ghost) {
    return Mp() ? "Ghost: not in multiplayer"
         : Fn<uint32_t __cdecl()>(site::WreckingCrew)() & 0xFF ? "Ghost: not in Wrecking Crew"
         : ghost && Field<unsigned>(me, 0xAC4) != ~0u ? "Ghost: get out of the vehicle first" : nullptr;
}

void Toggle(uint8_t* me) {
    if (g_on) {
        g_on = false;
        Destroy();
        Hud("Ghost off");
        return;
    }
    if (g_net) return Hud("Ghost: not during a co-op session");
    if (const char* why = Refused(me, true)) return Hud(why);
    if (!g_installed && !(g_installed = Install())) return Hud("Ghost unavailable: unexpected game code (RfgCoop.log)");
    g_on = true;
    Hud("Ghost on: it follows you 0.4 s behind");
}

void Stats(uint8_t* me, uint8_t* p) { // log every 5 s; the asm caves report their first hits here
    static long seen[7];
    const long hits[7] = {gc_airHits, gc_meleeHits, gc_crouchSkips, gc_shakeKeeps, gc_rippleSkips, gc_rippleSpared, gc_cutterSkips};
    static const char* const names[7] = {"PP-AIR: airborne puppet read its own command",
                                         "PP-MELEE: puppet lunge along its own heading", "PP-CROUCH: your camera framing kept",
                                         "G5: your camera shakes kept as its pack ended",
                                         "G3: its ripple moved objects only (no NPC or vehicle hit)",
                                         "G4: a ripple spared a co-op player", "G7: its end-of-charge cutter skipped"};
    for (int i = 0; i < 7; i++)
        if (hits[i] != seen[i] && seen[i] < 3) seen[i]++, Log("%s: %s (hit %ld)", Who(), names[i], hits[i]);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    double now = t.QuadPart * 1000.0 / g_qpcFreq;
    if (now - g_lastStat < 5000) return;
    g_lastStat = now;
    vector a = g_isPartner ? g_partnerWhere : Field<vector>(me, 0x04), b = Field<vector>(p, 0x04);
    float dx = b.x - a.x - (g_isPartner ? 0 : g_offset), dy = b.y - a.y, dz = b.z - a.z;
    Log("%s: %s %.2f m, puppet states %d, ring %d, delivered %u, visible %d, hp %.0f, clock offset %d ms; air %ld melee %ld "
        "crouch-skips %ld",
        Who(), g_isPartner ? "puppet behind the partner's last PSTATE by" : "player-puppet distance (offset removed)",
        sqrtf(dx * dx + dy * dy + dz * dz), Field<int>(p, 0x1274), g_count, g_delivered, g_visible, Field<float>(p, 0x914),
        g_map.off, gc_airHits, gc_meleeHits, gc_crouchSkips);
}

void ClockTick() { // the MP clock, frozen in SP: QPC ms from 10000 at the first install, unsigned, monotonic
    if (!g_installed || Mp()) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!g_qpc0) g_qpc0 = t.QuadPart;
    *Rva<uint32_t>(site::Clock) = 10000u + (uint32_t)((t.QuadPart - g_qpc0) * 1000 / g_qpcFreq);
}

// ---- partner input ---------------------------------------------------------------------------------------------------
// The sender's capture time -> local Clock: offset = arrival - sent, smoothed. The target is the smallest offset of the
// last 2 s window (the fastest packet: least queueing), and the used offset slews toward it by at most 1 ms per state,
// so jitter neither compresses nor reorders network_time; + kInterpMs of interpolation delay. A drop over 2 s re-syncs
// at once; a rise is queueing (this sim thread stalled and now drains its inbox, a delay spike), never a new mapping:
// within a link the sender's QPC Clock never steps back, and a partner restart is a new link (Puppet_Net clears g_map).
// Such stale states map into the past and Deliver's monotonic clamp absorbs them.
uint32_t MapTime(uint32_t sent, uint32_t now) {
    int32_t off = (int32_t)(now - sent);
    if (!g_map.have || off - g_map.off < -2000) {
        Log("partner: clock offset %s at %d ms", g_map.have ? "re-synced" : "set", off);
        g_map = {true, off, off, off, now};
    }
    if (off < g_map.winMin) g_map.winMin = off;
    if ((int32_t)(now - g_map.winStart) >= 2000) g_map.target = g_map.winMin, g_map.winMin = off, g_map.winStart = now;
    g_map.off += g_map.target > g_map.off ? 1 : g_map.target < g_map.off ? -1 : 0;
    return sent + (uint32_t)g_map.off + kInterpMs;
}

bool InRange(const float* f, int n, float lim) { // finite and |f| <= lim (NaN fails the compare)
    for (int i = 0; i < n; i++)
        if (!(fabsf(f[i]) <= lim)) return false;
    return true;
}
float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
bool Rotation(const float* m) { // rows of unit length and pairwise orthogonal, within 0.1 (an honest object orient is)
    for (int i = 0; i < 3; i++)
        for (int j = i; j < 3; j++)
            if (!(fabsf(m[i * 3] * m[j * 3] + m[i * 3 + 1] * m[j * 3 + 1] + m[i * 3 + 2] * m[j * 3 + 2] - (i == j)) <= 0.1f))
                return false;
    return true;
}
bool Streamed(const vector* v) { // world::is_zone_streamed: unsigned grid bounds, so off-grid or NaN is false
    return Fn<uint32_t __thiscall(void*, const vector*)>(site::ZoneStreamed)(Rva(rva::World), v) & 0xFF;
}

void* ItemInfo(uint32_t h) { // the sender's weapon by its inv_item_info handle, as inv_item_info_from_handle 0x612370
    if (h == ~0u) return nullptr;
    uint8_t* t = Rva<uint8_t>(site::InvItemInfo);
    for (int i = 0, n = *Rva<int>(site::NumInvItemInfo); i < n && i < 64; i++)
        if (Field<uint32_t>(t + i * 0x28, 0x04) == h) return t + i * 0x28;
    if (++g_nItem <= 3) Log("partner: unknown weapon handle %08X, shown unarmed (#%d)", h, g_nItem);
    return nullptr;
}

Snap Hidden(uint32_t time, uint32_t now) { // the partner puppet stands still, hidden, where it was last seen
    Snap s = g_lastVis;
    Still(s.st);
    s.st.time = time, s.due = now, s.visible = false;
    return s;
}

} // namespace

void Puppet_Install() {
    g_dev = IniInt(L"dev", 0) == 1;
    g_l5 = IniInt(L"l5", 1) != 0;
    g_ppStats = IniInt(L"pp_stats", 1) != 0;
    g_offset = (float)IniInt(L"ghost_offset", 2);
    g_icon = IniInt(L"partner_icon", 1) != 0;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = f.QuadPart;
    if (g_dev) Log("ghost: F6 toggles the ghost (l5 %d, offset %.0f m); nothing is patched before the first F6", g_l5, g_offset);
    if (g_dev && !g_ppStats) // multi_game_stats_are_finalizing via record_weapon_time (0x6E6B70), NULL Mp_game_data
        Log("ghost: PP-STATS off (control run): expect AV at rfg+5862E6 reading 0x610 on the first puppet frame");
}

void Puppet_Tick(int state) {
    uint8_t* me = LocalPlayer();
    if (g_dev) {
        bool f6 = *Rva<uint8_t>(rva::OsAppActive) && GetAsyncKeyState(VK_F6) < 0;
        if (f6 && !g_f6 && state == 1 && me) Toggle(me);
        g_f6 = f6;
    }
    ClockTick();
    bool want = g_on || g_net;
    if (gc_puppet && (!want || g_isPartner != g_net)) { // left over: the ghost when a session links, the partner after it
        if (state == 1) Destroy(); // in gameplay, as F6
        else Puppet();             // elsewhere only forgotten once the game freed it (level unload)
        return;
    }
    if (!want) return;
    if (uint8_t* p = Puppet()) {
        if (me) Stats(me, p);
        return;
    }
    if (state != 1 || !me || !Ready(me) || Refused(me, g_on)) return;
    if (g_on) {
        vector pos = Field<vector>(me, 0x04);
        pos.x += g_offset;
        if (!Create(me, pos, Field<matrix>(me, 0x10))) g_on = false, Hud("Ghost failed (RfgCoop.log)");
    } else if (g_lastInVisible && !g_partnerLoading && ClockNow() - g_lastVisAt < 1000 && g_createFails < 3) {
        // created where the partner is, from its latest visible state; the next PSTATEs drive it
        if (!Create(me, g_lastVis.st.pos, g_lastVis.st.orient) && ++g_createFails == 3)
            Hud("Partner puppet failed (RfgCoop.log)");
    }
}

// ---- session.cpp ------------------------------------------------------------------------------------------------------
bool Puppet_Net(bool on) {
    if (!on) {
        if (!g_net) return false;
        g_net = false, g_lastInVisible = false, g_lastVisAt = g_partnerAt = 0;
        L5(gc_puppet != nullptr); // the partner puppet itself goes in Puppet_Tick (in gameplay or with the level)
        Log("partner: session down, partner puppet removed");
        return true;
    }
    if (!g_installed && !(g_installed = Install()))
        return Log("partner: game code differs (above): linked without a puppet, nothing captured"), false;
    g_on = false; // the ghost, if any, goes in Puppet_Tick
    // a partner puppet left from the last link (not removed yet) is kept for the new partner: re-seed its ids
    g_net = true, g_partnerLoading = g_lastInVisible = g_detHave = g_bpHave = g_seeded = false, g_map.have = false;
    g_bpResync = true;
    gc_ppWorld = Guest_Armed() ? kPolicyGuest : kPolicyHost; // session.cpp Link arms the guest before this call
    Log("partner: pack gate policy %d as %s (1 world, 2 NPCs, 4 vehicles, 8 live epoch)", gc_ppWorld, Guest_Armed() ? "guest" : "host");
    g_lastVisAt = g_partnerAt = 0, g_capPlayer = nullptr, g_out = {}, g_outFresh = g_bpRepeat = false, g_createFails = 0;
    ClockTick();
    L5(true);
    Log("partner: session linked: capturing you for PSTATE; the partner puppet appears with its first visible state");
    return true;
}

uint32_t Puppet_Handle() { return g_net && g_isPartner && gc_puppet ? g_handle : ~0u; }

void Puppet_SetWorldBit(bool on) { // guest, per epoch (M6 §6): off = bit 8 (G6 passes buildings); bit 0 (G7) untouched
    if (!(gc_ppWorld & 8) == on) return;
    gc_ppWorld = on ? gc_ppWorld & ~8 : gc_ppWorld | 8;
    Log("partner: pack gate policy %d (world bit %s: %s)", gc_ppWorld, on ? "on" : "off",
        on ? "the host's puppet breaks this world, as its guns"
           : "its ram passes buildings, its end cutter's holes are dropped: the holes come from the host's stream");
}

bool Puppet_Out(MsgPState& m, bool paused) {
    uint8_t* me = LocalPlayer();
    // level gone: the next Player may reuse the pool slot (same pointer), Capture resets; no repeat into the next level
    if (!me) g_capPlayer = nullptr, g_bpRepeat = false;
    if (!g_net || !me || me != g_capPlayer || !g_out.at) return false; // nothing captured in this level yet
    uint32_t now = ClockNow();
    Snap s = g_out;
    if (paused || !g_outFresh) { // paused (Esc, map, menus: Capture may still run), or no capture since: a hitch
        if (!paused && now - s.at < 150) return false;
        Still(s.st), s.at = now; // stand still (re-stamped) rather than let the partner extrapolate your last move
        s.bp |= kFrozen;         // and the partner ends your pack until you capture again (backpack_sync.md §3.3)
    }
    // A step with level 1 repeats that level in the next (unfrozen) state: if the step's PSTATE is lost (unreliable
    // channel, or late and dropped), the next state carries the step itself, and a low down-Thrust has level 0 there.
    bool repeat = g_bpRepeat && !(s.bp & kFrozen);
    g_bpRepeat = !(s.bp & kFrozen) && ((s.bp ^ g_sentBp) & 0xF) && (s.bp & kLevel);
    if (repeat) s.bp |= kLevel;
    g_outFresh = false;
    const MpState& st = s.st;
    m = {};
    m.type = kPState, m.flags = s.visible, m.time = s.at;
    memcpy(m.where, &g_outWhere, 12), memcpy(m.pos, &st.pos, 12), memcpy(m.vel, &st.vel, 12);
    memcpy(m.orient, &st.orient, 36), memcpy(m.cam, &st.camOrient, 36);
    m.forward = st.forward, m.sideways = st.sideways, m.hp = st.hpPct;
    m.ladder = st.ladder, m.ragdoll = st.ragdoll, m.knockdown = st.knockdown, m.ladderState = st.ladderState;
    m.det = (g_detCharges & 0xF) | (g_detAir & 0xF) << 4;
    m.pack = s.pack;
    m.bits = st.bits;
    m.item = s.info ? Field<uint32_t>(s.info, 0x04) : ~0u; // inv_item_info_get_handle 0x612360
    m.bpAct = g_sentBp = s.bp, m.bpFuel = s.fuel;
    return true;
}

// The capture that stepped the pack counter goes out at once (SendState skips its 50 ms gap for it), with level 1 and
// its in-air pose: a low down-Thrust (boost timer 0 in the air, 0x5F2285) can start and end within one gap, and the
// latest capture alone would carry the step with level 0, which the receiver drops as stale. One extra PSTATE at most.
// Puppet_Out repeats the step's level 1 in the next state, so losing this one PSTATE loses no Thrust.
bool Puppet_Step() { return g_outFresh && (g_out.bp ^ g_sentBp) & 0xF; }

void Puppet_In(const MsgPState& m) {
    if (!g_net || g_partnerLoading) return; // after LOADING, nothing until READY (it re-seeds the ids)
    // trust boundary: every float finite and in range, orient and cam rotations (orient reaches obj_human_props and
    // update_pos_and_orient at creation; process_state 0x5E1D06 runs matrix::orthogonalize on cam, which divides by its
    // f row's length unguarded, 0x1A26C1), the ragdoll enum in range; handles never come from the wire
    if (!InRange(m.where, 3, 1e5f) || !InRange(m.pos, 3, 1e5f) || !InRange(m.vel, 3, 1e3f) || !InRange(m.orient, 9, 1.5f) ||
        !Rotation(m.orient) || !InRange(m.cam, 9, 1.5f) || !Rotation(m.cam) || !(fabsf(m.forward) <= 10) ||
        !(fabsf(m.sideways) <= 10) || !(fabsf(m.hp) <= 10) || (uint32_t)m.ragdoll > 6) { // NUM_HUMAN_RAGDOLL_STATES = 7
        if (++g_nBad <= 3) Log("partner: invalid PSTATE dropped (#%d)", g_nBad);
        return;
    }
    uint32_t now = ClockNow(), time = MapTime(m.time, now);
    memcpy(&g_partnerWhere, m.where, 12), g_partnerAt = now;
    uint8_t* me = LocalPlayer();
    if (GameState() != 1 || !me || !Ready(me)) return void(g_bpAway = true); // loading or a menu: no world reads; the puppet is gone or paused
    // 250 ms+ late (a delay spike), or due already after 250 ms without a puppet frame (your own stall drains its inbox
    // in one frame, before the puppet moves from its pre-stall place): as after your menu
    if ((int32_t)(now - time) > 150 || ((int32_t)(now - time) >= 0 && now - g_simAt > 250)) g_bpAway = true;
    Snap s{};
    MpState& st = s.st;
    memcpy(&st.pos, m.pos, 12), memcpy(&st.vel, m.vel, 12), memcpy(&st.orient, m.orient, 36), memcpy(&st.camOrient, m.cam, 36);
    st.time = time;
    st.forward = Clamp(m.forward, -1.5f, 1.5f), st.sideways = Clamp(m.sideways, -1.5f, 1.5f), st.hpPct = Clamp(m.hp, 0, 1.5f);
    st.ragdoll = m.ragdoll, st.knockdown = m.knockdown < 0 ? 0 : m.knockdown > 10000 ? 10000 : m.knockdown;
    st.turretNet = st.vehNet = 0xFFFF, st.turretGame = st.vehGame = ~0u; // never from the wire (the SP divide-by-zero trap)
    st.bits = m.bits & 0x7FFF3FFFu;      // only what Capture writes (bits 0-13, 16-30)
    st.ladder = ~0u;
    if (m.ladder != ~0u) // apply_movement_state mounts only a local object of type 0x11 (0x5CF474-0x5CF497)
        if (uint8_t* l = ObjectFromHandle(m.ladder); l && Field<uint8_t>(l, 0x7E) == 0x11)
            st.ladder = m.ladder, st.ladderState = (m.ladderState & 0xFF) <= 1 ? m.ladderState : 0;
    if (st.ladder == ~0u) st.bits &= ~0x40u; // no such ladder here: no climb
    uint8_t det = g_detHave ? m.det ^ g_detSeen : 0; // a counter step = one detonation; the first state only sets the base
    st.bits = (st.bits & ~0x2800u) | (det & 0x0F ? 0x2000u : 0) | (det & 0xF0 ? 0x800u : 0);
    g_detSeen = m.det, g_detHave = true;
    // A pack counter step = an activation; the first state sets the base. A step seen first after your menu, map or load,
    // in a state 250 ms+ late or due after 250 ms without a puppet frame (your own stall drains its inbox in one frame,
    // at the puppet's old place), or after a loss burst (5+ states lost in a row, 250 ms at 20 Hz: it may be from
    // anywhere in the gap; seq, not time, so a sender's own hitch, which skips no seq and maps on time, is neither) is
    // kStale: PackEffects resyncs on it, so a Shockwave fired meanwhile is lost (as while hidden, backpack_sync.md §3.3) rather than a late blast at the puppet's current place,
    // and a Thrust is not launched mid-flight; a pack still on is activated.
    static uint16_t prevSeq;
    uint16_t lost = uint16_t(m.seq - prevSeq - 1); // the session drops late or duplicate states (session.cpp OnMessage)
    bool step = g_bpHave && (m.bpAct ^ g_bpSeen) & 0xF, burst = step && !g_bpAway && lost >= 5;
    if (burst) Log("partner: pack step after %u lost states: a resync, not an edge", lost);
    s.bp = uint8_t((m.bpAct & (kLevel | kUp | kLanded | kFrozen)) | (step ? (g_bpAway || burst ? kStale : kEdge) : 0) |
                   (g_bpAway ? kAway : 0));
    s.fuel = m.bpFuel;
    g_bpSeen = m.bpAct, g_bpHave = true, g_bpAway = false, prevSeq = m.seq;
    s.info = ItemInfo(m.item), s.due = now;
    s.pack = m.pack <= 10 ? m.pack : 0; // ids 0..9, the SP packs (picker_equip.md §3); create also bounds-checks
    // Visible: on foot, and both where and pos streamed here and within 10 m of each other (an honest pos is where, or
    // the ragdoll body). The puppet is made and driven at pos; a hidden state never uses the received pos (Hidden).
    vector d{st.pos.x - g_partnerWhere.x, st.pos.y - g_partnerWhere.y, st.pos.z - g_partnerWhere.z};
    s.visible = (m.flags & 1) && Streamed(&g_partnerWhere) && Streamed(&st.pos) && d.x * d.x + d.y * d.y + d.z * d.z <= 10 * 10;
    if ((g_lastInVisible = s.visible)) g_lastVis = s, g_lastVisAt = now;
    else if (!g_lastVisAt && !(gc_puppet && g_isPartner)) return; // never seen and no puppet kept from the last link
    else {
        // a puppet kept across a relink stands hidden where the last link left it, with the new link's ids and weapon
        if (!g_lastVisAt) g_lastVis.st.bits = st.bits, g_lastVis.info = s.info;
        s = Hidden(time, now); // in a vehicle or out of your streamed zones
    }
    if (gc_puppet && g_isPartner) Push(s);
}

void Puppet_PartnerLoading(bool loading) {
    if (!g_net || loading == g_partnerLoading) return;
    g_partnerLoading = loading, g_lastInVisible = false;
    if (!loading) { // a new level on the partner's side: new Player, new ids
        g_seeded = g_detHave = g_bpHave = false, g_bpResync = true;
        return;
    }
    uint32_t now = ClockNow();
    if (gc_puppet && g_isPartner) Push(Hidden(now + kInterpMs, now)); // g_lastVis is valid while a partner puppet exists
}

bool Puppet_PartnerPos(float pos[3]) {
    if (!g_net || g_partnerLoading || !g_partnerAt || ClockNow() - g_partnerAt > 10000) return false;
    memcpy(pos, &g_partnerWhere, 12);
    return true;
}
