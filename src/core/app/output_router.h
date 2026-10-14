// MOD-05 OutputRouter (spec §9, §11, IF-06): logical outputs -> physical displays by STABLE IDENTITY.
//  * Display index is never identity. Reconnect maps only to the same stableId.
//  * An audience/program output never auto-covers the controls display (Blocked), whatever the display set does.
//  * Identical monitors (ambiguous) stay idle (NeedsReassign) until the operator re-confirms the route after any topology change.
//  * A lost display marks the route Disconnected; nothing else moves. Role Controls hosts panels and produces no frames.
//  * Final visual blackout is applied to the LAST composition (finalize()), after rendering, on audience/program outputs only.
// Thread-safe (single mutex; blackout flag is atomic and lock-free for the render thread).
#pragma once
#include "core/app/app_types.h"
#include <atomic>
#include <mutex>

namespace ss { namespace app {

struct RouteDesc {                              // immutable per table generation
  std::string logicalId;
  int outputIndex = -1;                         // index in the show document's output list
  show::OutputRole role = show::OutputRole::Audience;
  show::OverlayPolicy overlay;
  std::string displayId;                        // "" for virtual program
  int x = 0, y = 0, w = 0, h = 0, rotation = 0;
  RouteState state = RouteState::Unassigned;
  uint64_t generation = 0;
  bool diagnostics = false;
  bool rendersFrames() const { return (state == RouteState::Active || state == RouteState::Virtual) && role != show::OutputRole::Controls; }
};
struct RouteTable { uint64_t generation = 0; std::vector<RouteDesc> routes; };

class OutputRouter {
 public:
  void setOutputs(const std::vector<show::Output>& outs);                         // document change; keeps assignments by logicalId
  void setAssignments(const std::map<std::string, std::string>& logicalToDisplay);// persisted LocalSettings.outputDisplays
  std::map<std::string, std::string> assignments() const;
  void setControlsDisplay(const std::string& stableId);                           // "" = infer (primary) on next display update
  std::string controlsDisplay() const;
  void updateDisplays(const std::vector<DisplayInfo>& current);                   // WM_DISPLAYCHANGE result
  // Operator route selection. Rejects: unknown output, controls role, controls display, display already used by another output.
  // Identical monitors: the chosen DisplayInfo (position) is remembered so the route can go Active; any later topology change
  // returns it to NeedsReassign.
  Status assign(const std::string& logicalId, const DisplayInfo& d);
  Status assignById(const std::string& logicalId, const std::string& stableId);   // fails for ambiguous identities
  Status unassign(const std::string& logicalId);
  void setDiagnostics(const std::string& logicalId, bool on);
  bool diagnostics(const std::string& logicalId) const;

  void setBlackout(bool on) { blackout_.store(on, std::memory_order_release); }
  bool blackout() const { return blackout_.load(std::memory_order_acquire); }
  // Applies the final blackout composition. Returns true if the frame was replaced by black.
  bool finalize(const RouteDesc& r, Frame& f) const;

  std::shared_ptr<const RouteTable> table() const;
  std::vector<OutputRouteView> views() const;     // controller fills scene/frames fields
  uint64_t generation() const;
  std::vector<DisplayInfo> displays() const;
  static std::string identifyLabel(const DisplayInfo& d);

 private:
  struct Conf { std::string stableId; int x = 0, y = 0, w = 0, h = 0; std::string topoSig; bool valid = false; };
  struct Slot { RouteDesc desc; OutputRouteView view; };
  void rebuildLocked();
  std::string topoSigLocked() const;
  mutable std::mutex m_;
  std::vector<show::Output> outs_;
  std::map<std::string, std::string> assigned_;  // logicalId -> display stableId
  std::map<std::string, Conf> confirmed_;        // ambiguous confirmations (session only)
  std::map<std::string, bool> diag_;
  std::vector<DisplayInfo> displays_;
  bool haveDisplays_ = false;
  std::string controls_;
  bool controlsInferred_ = false;
  std::vector<Slot> slots_;
  std::shared_ptr<const RouteTable> table_ = std::make_shared<RouteTable>();
  uint64_t gen_ = 0;
  std::atomic<bool> blackout_{false};
};

}} // namespace ss::app
