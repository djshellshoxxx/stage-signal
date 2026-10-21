#include "helpers.hpp"

using namespace testutil;

TEST_CASE("REQ-M01: learn consumes the next supported message for the chosen device and channel") {
    LearnSession ls;
    PortInfo a{1, "usb:A", "Controller A", 1, true, 0, true};
    PortInfo b{2, "usb:B", "Controller B", 1, true, 0, true};
    LearnFilter f; f.anyPort = false; f.portId = 1; f.channel = 4;
    ls.begin(f);
    CHECK_FALSE(ls.offer(a, makeRealtime(MsgKind::Clock)));       // not a supported message
    CHECK_FALSE(ls.offer(a, makeNoteOff(4, 60)));                 // a stray release must not bind
    CHECK_FALSE(ls.offer(b, makeCC(4, 7, 50)));                   // wrong device
    CHECK_FALSE(ls.offer(a, makeCC(5, 7, 50)));                   // wrong channel
    CHECK_FALSE(ls.captured());
    CHECK(ls.offer(a, makeCC(4, 21, 30)));                        // consumed
    REQUIRE(ls.captured());
    CHECK(ls.capture()->number == 21);
    CHECK(ls.capture()->type == model::MidiMsgType::ControlChange);
    CHECK(ls.capture()->deviceIdentity == "usb:A");
    CHECK_FALSE(ls.offer(a, makeNoteOn(4, 60, 100)));             // only the NEXT message is consumed
}

TEST_CASE("REQ-M01: learn shows normalised range and current value, then commits a mapping") {
    LearnSession ls;
    PortInfo a{1, "usb:A", "Controller A", 1, true, 0, true};
    ls.begin();
    REQUIRE(ls.offer(a, makeCC(0, 7, 20)));
    CHECK(ls.offer(a, makeCC(0, 7, 100)));
    CHECK(ls.offer(a, makeCC(0, 7, 60)));
    CHECK_FALSE(ls.offer(a, makeCC(0, 8, 60)));                   // other control is not tracked
    const LearnCapture* c = ls.capture();
    REQUIRE(c);
    CHECK(c->observedMin == 20);
    CHECK(c->observedMax == 100);
    CHECK(c->currentValue == 60);
    CHECK(c->normalisedCurrent() == doctest::Approx(60.0 / 127.0));
    ls.useObservedRange();
    CHECK(c->normalisedCurrent() == doctest::Approx(0.5));
    CHECK(c->normalisedObservedMin() == doctest::Approx(0.0));
    CHECK(c->normalisedObservedMax() == doctest::Approx(1.0));
    auto m = ls.commit(ActionId::Brightness);
    REQUIRE(m.ok());
    CHECK(m.value().valueMin == 20);
    CHECK(m.value().valueMax == 100);
    CHECK(m.value().actionId == id(ActionId::Brightness));
    CHECK_FALSE(ls.active());
}

TEST_CASE("REQ-M01: learn binds notes and program changes; relative mode on a note is refused") {
    PortInfo a{1, "usb:A", "Controller A", 1, true, 0, true};
    LearnSession ls;
    ls.begin();
    REQUIRE(ls.offer(a, makeNoteOn(9, 36, 127)));
    CHECK(ls.capture()->type == model::MidiMsgType::Note);
    CHECK_FALSE(ls.commit(ActionId::Brightness, model::EncoderMode::RelativeSignBit).ok());
    CHECK(ls.commit(ActionId::NextScene).ok());
    ls.begin();
    REQUIRE(ls.offer(a, makeProgramChange(0, 5)));
    CHECK(ls.capture()->type == model::MidiMsgType::ProgramChange);
    CHECK(ls.capture()->number == 5);
}

TEST_CASE("REQ-M01: learn consumes the message so it does not also run a mapped action") {
    Rig r;
    r.add(model::MidiMsgType::Note, 36, ActionId::NextScene);
    r.e.learn().begin();
    r.noteOn(36, 100, 0);
    CHECK(r.poll(0).empty());
    CHECK(r.e.learn().captured());
    CHECK(r.e.totals().learnConsumed == 1);
}

TEST_CASE("REQ-M01: mapping inspect, edit and delete roundtrip") {
    MappingTable t;
    auto id1 = t.add(mkMap(model::MidiMsgType::Note, 36, ActionId::NextScene));
    auto id2 = t.add(mkMap(model::MidiMsgType::ControlChange, 7, ActionId::Brightness));
    REQUIRE(id1.ok());
    REQUIRE(id2.ok());
    CHECK(id1.value() != id2.value());
    REQUIRE(t.find(id1.value()));
    CHECK(t.find(id1.value())->map.number == 36);

    auto edited = mkMap(model::MidiMsgType::Note, 37, ActionId::PrevScene);
    REQUIRE(t.update(id1.value(), edited).ok());
    CHECK(t.find(id1.value())->map.number == 37);
    CHECK(t.find(id1.value())->map.actionId == id(ActionId::PrevScene));
    CHECK(t.find(id1.value())->map.id == id1.value());

    // export to the show document and import again
    auto doc = t.toModel();
    REQUIRE(doc.size() == 2);
    MappingTable t2;
    REQUIRE(t2.load(doc).ok());
    CHECK(t2.toModel().size() == 2);
    CHECK(t2.find(id2.value())->map.number == 7);

    REQUIRE(t.remove(id1.value()).ok());
    CHECK(t.find(id1.value()) == nullptr);
    CHECK(t.size() == 1);
    CHECK_FALSE(t.remove(id1.value()).ok());
    CHECK_FALSE(t.update(999, edited).ok());
}

TEST_CASE("REQ-M01: invalid mappings are rejected and the table is bounded to 512") {
    MappingTable t;
    auto bad = mkMap(model::MidiMsgType::Note, 1, ActionId::NextScene);
    bad.channel = 16;                       CHECK_FALSE(t.add(bad).ok());
    bad = mkMap(model::MidiMsgType::Note, 200, ActionId::NextScene);       CHECK_FALSE(t.add(bad).ok());
    bad = mkMap(model::MidiMsgType::Note, 1, ActionId::None);               CHECK_FALSE(t.add(bad).ok());
    bad = mkMap(model::MidiMsgType::Note, 1, ActionId::NextScene);
    bad.deviceIdentity.clear(); bad.deviceName.clear();                     CHECK_FALSE(t.add(bad).ok());
    bad = mkMap(model::MidiMsgType::ControlChange, 1, ActionId::Brightness);
    bad.valueMin = 100; bad.valueMax = 10;                                  CHECK_FALSE(t.add(bad).ok());
    // relative encoders: CC + continuous action only
    CHECK_FALSE(t.add(mkMap(model::MidiMsgType::Note, 1, ActionId::Brightness, model::EncoderMode::RelativeBinaryOffset)).ok());
    CHECK_FALSE(t.add(mkMap(model::MidiMsgType::ControlChange, 1, ActionId::NextScene, model::EncoderMode::RelativeBinaryOffset)).ok());
    CHECK(t.size() == 0);
    for (std::size_t i = 0; i < model::Limits::kMaxMidiMappings; ++i)
        REQUIRE(t.add(mkMap(model::MidiMsgType::Note, static_cast<std::uint8_t>(i % 128), ActionId::NextScene)).ok());
    CHECK_FALSE(t.add(mkMap(model::MidiMsgType::Note, 1, ActionId::NextScene)).ok());
    CHECK(t.size() == 512);
}

TEST_CASE("REQ-M01: relative encoder modes decode explicitly (two's complement, binary offset, sign bit)") {
    using model::EncoderMode;
    CHECK(relativeDelta(EncoderMode::RelativeTwosComplement, 1) == 1);
    CHECK(relativeDelta(EncoderMode::RelativeTwosComplement, 63) == 63);
    CHECK(relativeDelta(EncoderMode::RelativeTwosComplement, 127) == -1);
    CHECK(relativeDelta(EncoderMode::RelativeTwosComplement, 64) == -64);
    CHECK(relativeDelta(EncoderMode::RelativeBinaryOffset, 64) == 0);
    CHECK(relativeDelta(EncoderMode::RelativeBinaryOffset, 65) == 1);
    CHECK(relativeDelta(EncoderMode::RelativeBinaryOffset, 63) == -1);
    CHECK(relativeDelta(EncoderMode::RelativeBinaryOffset, 0) == -64);
    CHECK(relativeDelta(EncoderMode::RelativeSignBit, 1) == 1);
    CHECK(relativeDelta(EncoderMode::RelativeSignBit, 0x41) == -1);
    CHECK(relativeDelta(EncoderMode::RelativeSignBit, 0x40) == 0);
    CHECK(relativeDelta(EncoderMode::RelativeSignBit, 0x7F) == -63);
    CHECK(relativeDelta(EncoderMode::Absolute, 99) == 0);
    // the same byte means different things in different modes: nothing is guessed
    CHECK(relativeDelta(EncoderMode::RelativeTwosComplement, 0x41) != relativeDelta(EncoderMode::RelativeSignBit, 0x41));
}

TEST_CASE("REQ-M01: relative encoders accumulate into an absolute normalised value") {
    Rig r;
    MappingOptions opt; opt.relativeStep = 0.1; opt.initialValue = 0.5;
    r.add(model::MidiMsgType::ControlChange, 16, ActionId::Brightness, model::EncoderMode::RelativeBinaryOffset, 0, opt);
    r.cc(16, 65, 0); r.cc(16, 65, 1); r.cc(16, 65, 2);          // +3 ticks
    auto a = r.poll(3);
    REQUIRE(a.size() == 1);                                      // coalesced to the latest value
    CHECK(a[0].value == doctest::Approx(0.8));
    r.cc(16, 60, 4);                                             // -4 ticks
    a = r.poll(5);
    REQUIRE(a.size() == 1);
    CHECK(a[0].value == doctest::Approx(0.4));
    for (int i = 0; i < 20; ++i) r.cc(16, 100, 10 + i);          // clamps at 1.0
    a = r.poll(40);
    REQUIRE(a.size() == 1);
    CHECK(a[0].value == doctest::Approx(1.0));
    r.cc(16, 64, 50);                                            // zero delta produces nothing
    CHECK(r.poll(51).empty());
}

TEST_CASE("REQ-M01: absolute values scale through the mapped value range") {
    auto m = mkMap(model::MidiMsgType::ControlChange, 1, ActionId::Brightness, model::EncoderMode::Absolute, 0, "usb:A", 20, 100);
    CHECK(normaliseAbsolute(m, 20) == doctest::Approx(0.0));
    CHECK(normaliseAbsolute(m, 60) == doctest::Approx(0.5));
    CHECK(normaliseAbsolute(m, 100) == doctest::Approx(1.0));
    CHECK(normaliseAbsolute(m, 5) == doctest::Approx(0.0));
    CHECK(normaliseAbsolute(m, 127) == doctest::Approx(1.0));
}

TEST_CASE("REQ-M01: mappings prefer stable device identity; name fallback needs operator confirmation") {
    Rig r;
    // same device name, different identity (e.g. a second identical controller)
    auto id = r.e.mappings().add(mkMap(model::MidiMsgType::Note, 10, ActionId::NextScene, model::EncoderMode::Absolute, 0, "usb:OLD")).value();
    r.noteOn(10, 100, 0);
    CHECK(r.poll(1).empty());                                      // not confirmed: ignored
    CHECK(r.e.totals().unconfirmedSkipped == 1);
    CHECK(r.e.mappingStatus(id).availability == MappingAvailability::NeedsNameConfirmation);
    REQUIRE(r.e.mappings().setNameFallbackConfirmed(id, true).ok());
    r.noteOn(10, 100, 100 * kNsPerMs);
    CHECK(r.poll(101 * kNsPerMs).size() == 1);
    CHECK(r.e.mappingStatus(id).availability == MappingAvailability::Available);
    // changing the device voids the confirmation
    auto edited = mkMap(model::MidiMsgType::Note, 10, ActionId::NextScene, model::EncoderMode::Absolute, 0, "usb:OTHER");
    REQUIRE(r.e.mappings().update(id, edited).ok());
    CHECK_FALSE(r.e.mappings().find(id)->nameFallbackConfirmed);
}

TEST_CASE("REQ-M01: channel filter and omni") {
    Rig r;
    r.add(model::MidiMsgType::Note, 1, ActionId::NextScene, model::EncoderMode::Absolute, 2);
    auto omni = mkMap(model::MidiMsgType::Note, 2, ActionId::PrevScene, model::EncoderMode::Absolute, 255);
    REQUIRE(r.e.mappings().add(omni).ok());
    r.noteOn(1, 100, 0, 3);                       CHECK(r.poll(1).empty());
    r.noteOn(1, 100, 100 * kNsPerMs, 2);          CHECK(r.poll(101 * kNsPerMs).size() == 1);
    r.noteOn(2, 100, 200 * kNsPerMs, 9);          CHECK(r.poll(201 * kNsPerMs).size() == 1);
}
