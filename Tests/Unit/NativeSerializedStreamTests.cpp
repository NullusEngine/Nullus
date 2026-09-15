#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <Math/Vector3.h>
#include <Math/BoundsInt.h>
#include <Math/RectInt.h>
#include <Math/Vector2Int.h>
#include <Math/Vector3Int.h>
#include <Reflection/Variant.h>

#include "GameObject.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Serialize/BinaryBuffer.h"
#include "Serialize/NativeSerializedStream.h"
#include "Serialize/PPtr.h"
#include "Serialize/SerializedSpecialTypes.h"

namespace NLS::Engine::Serialize::Tests
{
namespace
{
std::shared_ptr<SerializedTypeTree> MakeFixtureTree()
{
    auto tree = std::make_shared<SerializedTypeTree>();

    SerializedTypeTreeNode root;
    root.serializedName = "Fixture";
    root.typeName = "Fixture";
    root.kind = SerializedNodeKind::Generic;
    root.fixedByteSize = -1;
    const auto rootId = tree->AddNode(std::move(root));

    SerializedTypeTreeNode count;
    count.parent = rootId;
    count.serializedName = "count";
    count.aliases = {"legacyCount"};
    count.typeName = "int32_t";
    count.kind = SerializedNodeKind::Integer;
    count.numericKind = SerializedNumericKind::Int32;
    count.fixedByteSize = 4;
    const auto countId = tree->AddNode(std::move(count));

    SerializedTypeTreeNode title;
    title.parent = rootId;
    title.serializedName = "title";
    title.typeName = "std::string";
    title.kind = SerializedNodeKind::String;
    title.flags = SerializedNodeFlags::Align4After;
    const auto titleId = tree->AddNode(std::move(title));

    SerializedTypeTreeNode values;
    values.parent = rootId;
    values.serializedName = "values";
    values.typeName = "Array<int32_t>";
    values.kind = SerializedNodeKind::Generic;
    values.flags = SerializedNodeFlags::Array | SerializedNodeFlags::Align4After;
    const auto valuesId = tree->AddNode(std::move(values));

    SerializedTypeTreeNode element;
    element.parent = valuesId;
    element.serializedName = "data";
    element.typeName = "int32_t";
    element.kind = SerializedNodeKind::Integer;
    element.numericKind = SerializedNumericKind::Int32;
    element.fixedByteSize = 4;
    tree->AddNode(std::move(element));

    static_cast<void>(countId);
    static_cast<void>(titleId);
    tree->RebuildIndexes();
    return tree;
}
} // namespace

TEST(NativeSerializedStreamTests, BinaryBufferUsesLittleEndianAndRejectsTruncation)
{
    BinaryBufferWriter writer;
    writer.WriteU32(0x78563412u);
    writer.WriteU64(0x1122334455667788ULL);
    writer.WriteString("abc");
    writer.Align4();

    const std::vector<uint8_t> expected {
        0x12, 0x34, 0x56, 0x78,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x03, 0x00, 0x00, 0x00, 'a', 'b', 'c', 0x00};
    EXPECT_EQ(writer.bytes, expected);

    BinaryBufferReader reader(writer.bytes);
    uint32_t u32 = 0u;
    uint64_t u64 = 0u;
    std::string text;
    ASSERT_TRUE(reader.ReadU32(u32));
    ASSERT_TRUE(reader.ReadU64(u64));
    ASSERT_TRUE(reader.ReadString(text));
    ASSERT_TRUE(reader.Align4());
    EXPECT_EQ(u32, 0x78563412u);
    EXPECT_EQ(u64, 0x1122334455667788ULL);
    EXPECT_EQ(text, "abc");
    EXPECT_TRUE(reader.IsComplete());

    const std::array<uint8_t, 3u> truncated {0x03, 0x00, 0x00};
    BinaryBufferReader invalid(truncated);
    std::string ignored;
    EXPECT_FALSE(invalid.ReadString(ignored));
}

TEST(NativeSerializedStreamTests, TypeTreeBuildsStablePathsAndSchemaHash)
{
    const auto tree = MakeFixtureTree();
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(tree->GetRoot(), 0u);
    ASSERT_NE(tree->FindPath("Fixture"), nullptr);
    ASSERT_NE(tree->FindPath("Fixture.count"), nullptr);
    EXPECT_EQ(tree->FindPath("Fixture.legacyCount")->id, tree->FindPath("Fixture.count")->id);
    ASSERT_NE(tree->FindPath("Fixture.title"), nullptr);
    ASSERT_NE(tree->FindPath("Fixture.values"), nullptr);
    ASSERT_NE(tree->FindPath("Fixture.values.data"), nullptr);
    EXPECT_NE(tree->GetSchemaHash(), 0u);

    const auto fieldId = tree->FindPath("Fixture.count")->fieldId;
    ASSERT_NE(fieldId, 0u);
    EXPECT_EQ(tree->FindFieldId(fieldId)->serializedName, "count");
    EXPECT_EQ(tree->GetParent(tree->FindPath("Fixture.count")->id)->id, tree->GetRoot());
    EXPECT_EQ(tree->GetFirstChild(tree->GetRoot())->serializedName, "count");
    EXPECT_EQ(tree->GetNextSibling(tree->FindPath("Fixture.count")->id)->serializedName, "title");
    EXPECT_EQ(tree->GetTypeSignature(), tree->GetSchemaHash());
}

TEST(NativeSerializedStreamTests, CursorReadsWritesAndCapturesAStableSlice)
{
    const auto tree = MakeFixtureTree();
    auto stream = std::make_shared<SerializedByteStream>(tree);
    stream->SetBytes({0x2a, 0x00, 0x00, 0x00});

    auto cursor = NativeSerializedStreamCodec::ResolveCursor(stream, "Fixture.count");
    ASSERT_TRUE(cursor.has_value());
    EXPECT_TRUE(cursor->IsValid());
    EXPECT_EQ(cursor->GetByteOffset(), 0u);
    EXPECT_EQ(cursor->GetByteSize(), 4u);

    int32_t value = 0;
    ASSERT_TRUE(cursor->Read(&value, sizeof(value)));
    EXPECT_EQ(value, 42);

    const int32_t updated = 99;
    ASSERT_TRUE(cursor->Write(&updated, sizeof(updated)));
    ASSERT_TRUE(cursor->Read(&value, sizeof(value)));
    EXPECT_EQ(value, 99);

    const auto slice = cursor->Capture(tree->FindPath("Fixture.count")->fieldId);
    ASSERT_TRUE(slice.has_value());
    EXPECT_TRUE(slice->IsValid());
    EXPECT_EQ(slice->bytes, (std::vector<uint8_t> {0x63, 0x00, 0x00, 0x00}));
}

TEST(NativeSerializedStreamTests, ArrayCursorUsesEncodedCountAndElementOffsets)
{
    const auto tree = MakeFixtureTree();
    auto stream = std::make_shared<SerializedByteStream>(tree);
    stream->SetBytes({
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,
        0x0a, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00,
        0x1e, 0x00, 0x00, 0x00});

    const auto first = NativeSerializedStreamCodec::ResolveCursor(stream, "Fixture.values.Array.data[0]");
    const auto second = NativeSerializedStreamCodec::ResolveCursor(stream, "Fixture.values.Array.data[1]");
    const auto third = NativeSerializedStreamCodec::ResolveCursor(stream, "Fixture.values.Array.data[2]");
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(first->GetByteOffset(), 12u);
    EXPECT_EQ(second->GetByteOffset(), 16u);
    EXPECT_EQ(third->GetByteOffset(), 20u);
    EXPECT_EQ(first->GetByteSize(), 4u);
    EXPECT_FALSE(NativeSerializedStreamCodec::ResolveCursor(stream, "Fixture.values.Array.data[3]").has_value());
}

TEST(NativeSerializedStreamTests, ReferenceSlotsAreStableAndDeduplicated)
{
    SerializedReferenceTable references;
    SerializedReferenceEntry entry;
    entry.identifier = ObjectIdentifier::LocalObject(17);
    entry.resourcePath = "Assets/fixture.asset";
    entry.expectedTypeName = "GameObject";

    const auto first = references.Intern(entry);
    const auto second = references.Intern(entry);
    ASSERT_NE(first, 0u);
    EXPECT_EQ(first, second);
    ASSERT_NE(references.Find(first), nullptr);
    EXPECT_EQ(references.Find(first)->identifier.localIdentifierInFile, 17);
    EXPECT_EQ(references.Find(0u), nullptr);
}

TEST(NativeSerializedStreamTests, CodecRoundTripsScalarStringAndVectorPayloads)
{
    const auto scalarSchema = NativeSerializedStreamCodec::BuildTypeTree(
        NLS_TYPEOF(int32_t),
        {"value", false});
    ASSERT_NE(scalarSchema, nullptr);
    SerializedReferenceTable scalarReferences;
    std::vector<uint8_t> scalarBytes;
    ASSERT_TRUE(NativeSerializedStreamCodec::EncodeVariant(
        meta::Variant(int32_t {123}),
        *scalarSchema,
        scalarReferences,
        scalarBytes));
    const auto scalar = NativeSerializedStreamCodec::DecodeVariant(
        scalarBytes,
        *scalarSchema,
        NLS_TYPEOF(int32_t),
        scalarReferences);
    ASSERT_TRUE(scalar.has_value());
    EXPECT_EQ(scalar->GetValue<int32_t>(), 123);

    const auto stringSchema = NativeSerializedStreamCodec::BuildTypeTree(
        NLS_TYPEOF(std::string),
        {"text", false});
    ASSERT_NE(stringSchema, nullptr);
    SerializedReferenceTable stringReferences;
    std::vector<uint8_t> stringBytes;
    ASSERT_TRUE(NativeSerializedStreamCodec::EncodeVariant(
        meta::Variant(std::string("hello")),
        *stringSchema,
        stringReferences,
        stringBytes));
    EXPECT_EQ(stringBytes.size(), 12u);
    const auto string = NativeSerializedStreamCodec::DecodeVariant(
        stringBytes,
        *stringSchema,
        NLS_TYPEOF(std::string),
        stringReferences);
    ASSERT_TRUE(string.has_value());
    EXPECT_EQ(string->GetValue<std::string>(), "hello");

    const Maths::Vector3 vector {1.0f, 2.0f, 3.0f};
    const auto vectorSchema = NativeSerializedStreamCodec::BuildTypeTree(
        NLS_TYPEOF(Maths::Vector3),
        {"vector", false});
    ASSERT_NE(vectorSchema, nullptr);
    SerializedReferenceTable vectorReferences;
    std::vector<uint8_t> vectorBytes;
    ASSERT_TRUE(NativeSerializedStreamCodec::EncodeVariant(
        meta::Variant(vector),
        *vectorSchema,
        vectorReferences,
        vectorBytes));
    const auto decodedVector = NativeSerializedStreamCodec::DecodeVariant(
        vectorBytes,
        *vectorSchema,
        NLS_TYPEOF(Maths::Vector3),
        vectorReferences);
    ASSERT_TRUE(decodedVector.has_value());
    EXPECT_FLOAT_EQ(decodedVector->GetValue<Maths::Vector3>().z, 3.0f);
}

TEST(NativeSerializedStreamTests, CodecHonorsAbsoluteAlignmentForSlicedStringPayloads)
{
    const auto schema = NativeSerializedStreamCodec::BuildTypeTree(
        NLS_TYPEOF(std::string),
        {"value", false});
    ASSERT_NE(schema, nullptr);

    SerializedReferenceTable references;
    std::vector<uint8_t> stringBytes;
    constexpr size_t stringOffset = 1u; // A preceding bool makes the slice unaligned.
    ASSERT_TRUE(NativeSerializedStreamCodec::EncodeVariant(
        meta::Variant(std::string("hello")),
        *schema,
        references,
        stringBytes,
        nullptr,
        stringOffset));

    // The full stream is [bool][string + global Align4 padding][following sibling].
    // Decoding the string slice must retain the global offset rather than treating
    // its local byte zero as a new alignment boundary.
    ASSERT_EQ(stringBytes.size(), 11u);
    const auto decoded = NativeSerializedStreamCodec::DecodeVariant(
        stringBytes,
        *schema,
        NLS_TYPEOF(std::string),
        references,
        nullptr,
        stringOffset);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->GetValue<std::string>(), "hello");

    const auto localOriginDecode = NativeSerializedStreamCodec::DecodeVariant(
        stringBytes,
        *schema,
        NLS_TYPEOF(std::string),
        references);
    EXPECT_FALSE(localOriginDecode.has_value());
}

TEST(NativeSerializedStreamTests, CapturedSlicesCarryReferenceRelocations)
{
    auto tree = std::make_shared<SerializedTypeTree>();
    SerializedTypeTreeNode root;
    root.serializedName = "Fixture";
    root.kind = SerializedNodeKind::ObjectReference;
    root.typeName = "GameObject";
    root.fixedByteSize = 4;
    tree->AddNode(std::move(root));
    tree->RebuildIndexes();

    auto stream = std::make_shared<SerializedByteStream>(tree);
    auto references = std::make_shared<SerializedReferenceTable>();
    SerializedReferenceEntry entry;
    entry.identifier = ObjectIdentifier::LocalObject(9);
    const auto slot = references->Intern(entry);
    stream->SetReferences(references);
    stream->SetBytes({
        static_cast<uint8_t>(slot & 0xffu),
        static_cast<uint8_t>((slot >> 8u) & 0xffu),
        static_cast<uint8_t>((slot >> 16u) & 0xffu),
        static_cast<uint8_t>((slot >> 24u) & 0xffu)});

    const auto slice = stream->CaptureSlice(0u, 4u, tree->GetSchemaHash());
    ASSERT_TRUE(slice.has_value());
    ASSERT_EQ(slice->referenceRelocations.size(), 1u);
    EXPECT_EQ(slice->referenceRelocations.front().byteOffset, 0u);
    EXPECT_EQ(slice->referenceRelocations.front().referenceSlot, slot);
}

TEST(NativeSerializedStreamTests, CodecRoundTripsIntegerGeometryTypes)
{
    const auto vector2Schema = NativeSerializedStreamCodec::BuildTypeTree(NLS_TYPEOF(Maths::Vector2Int), {"value", false});
    const auto vector3Schema = NativeSerializedStreamCodec::BuildTypeTree(NLS_TYPEOF(Maths::Vector3Int), {"value", false});
    const auto rectSchema = NativeSerializedStreamCodec::BuildTypeTree(NLS_TYPEOF(Maths::RectInt), {"value", false});
    const auto boundsSchema = NativeSerializedStreamCodec::BuildTypeTree(NLS_TYPEOF(Maths::BoundsInt), {"value", false});
    ASSERT_NE(vector2Schema, nullptr);
    ASSERT_NE(vector3Schema, nullptr);
    ASSERT_NE(rectSchema, nullptr);
    ASSERT_NE(boundsSchema, nullptr);

    const auto roundTrip = [](const meta::Variant& source, const meta::Type& type)
    {
        auto schema = NativeSerializedStreamCodec::BuildTypeTree(type, {"value", false});
        SerializedReferenceTable references;
        std::vector<uint8_t> bytes;
        if (schema == nullptr || !NativeSerializedStreamCodec::EncodeVariant(source, *schema, references, bytes))
            return std::optional<meta::Variant> {};
        return NativeSerializedStreamCodec::DecodeVariant(bytes, *schema, type, references);
    };

    const auto vector2 = roundTrip(meta::Variant(Maths::Vector2Int(1, 2)), NLS_TYPEOF(Maths::Vector2Int));
    const auto vector3 = roundTrip(meta::Variant(Maths::Vector3Int(3, 4, 5)), NLS_TYPEOF(Maths::Vector3Int));
    const auto rect = roundTrip(meta::Variant(Maths::RectInt(6, 7, 8, 9)), NLS_TYPEOF(Maths::RectInt));
    const auto bounds = roundTrip(
        meta::Variant(Maths::BoundsInt {Maths::Vector3Int(1, 2, 3), Maths::Vector3Int(4, 5, 6)}),
        NLS_TYPEOF(Maths::BoundsInt));
    ASSERT_TRUE(vector2.has_value());
    ASSERT_TRUE(vector3.has_value());
    ASSERT_TRUE(rect.has_value());
    ASSERT_TRUE(bounds.has_value());
    EXPECT_EQ(vector2->GetValue<Maths::Vector2Int>().y, 2);
    EXPECT_EQ(vector3->GetValue<Maths::Vector3Int>().z, 5);
    EXPECT_EQ(rect->GetValue<Maths::RectInt>().height, 9);
    EXPECT_EQ(bounds->GetValue<Maths::BoundsInt>().size.z, 6);
}

TEST(NativeSerializedStreamTests, CodecRoundTripsNullPPtrWithZeroReferenceSlot)
{
    using GameObjectPPtr = PPtr<NLS::Engine::GameObject>;
    const auto type = NLS_TYPEOF(GameObjectPPtr);
    ASSERT_TRUE(type.IsValid());
    const auto schema = NativeSerializedStreamCodec::BuildTypeTree(type, {"value", false});
    ASSERT_NE(schema, nullptr);

    SerializedReferenceTable references;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(NativeSerializedStreamCodec::EncodeVariant(
        meta::Variant(GameObjectPPtr {}),
        *schema,
        references,
        bytes));
    ASSERT_EQ(bytes, (std::vector<uint8_t> {0u, 0u, 0u, 0u}));
    EXPECT_TRUE(references.GetEntries().empty());

    const auto decoded = NativeSerializedStreamCodec::DecodeVariant(
        bytes,
        *schema,
        type,
        references);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->GetValue<GameObjectPPtr>().IsNull());
}

TEST(NativeSerializedStreamTests, ReferenceSlotRelocationRewritesCollidingLocalSlots)
{
    auto tree = std::make_shared<SerializedTypeTree>();
    SerializedTypeTreeNode root;
    root.serializedName = "reference";
    root.typeName = "PPtr<GameObject>";
    root.kind = SerializedNodeKind::ObjectReference;
    root.fixedByteSize = 4;
    tree->AddNode(std::move(root));
    tree->RebuildIndexes();

    SerializedReferenceTable source;
    const auto sourceSlot = source.Intern({ObjectIdentifier::LocalObject(41), {}, "GameObject", false});
    ASSERT_EQ(sourceSlot, 1u);
    std::vector<uint8_t> bytes {1u, 0u, 0u, 0u};

    SerializedReferenceTable destination;
    const auto occupiedSlot = destination.Intern({ObjectIdentifier::LocalObject(99), {}, "GameObject", false});
    ASSERT_EQ(occupiedSlot, 1u);
    ASSERT_TRUE(NativeSerializedStreamCodec::RelocateReferenceSlots(
        bytes,
        *tree,
        source,
        destination));
    EXPECT_EQ(bytes, (std::vector<uint8_t> {2u, 0u, 0u, 0u}));
    ASSERT_NE(destination.Find(2u), nullptr);
    EXPECT_EQ(destination.Find(2u)->identifier.localIdentifierInFile, 41);
}

TEST(NativeSerializedStreamTests, CodecRoundTripsReflectedGenericStruct)
{
    using BoundingSphere = NLS::Render::Geometry::BoundingSphere;
    BoundingSphere source;
    source.position = Maths::Vector3(1.0f, 2.0f, 3.0f);
    source.radius = 4.0f;
    const auto type = NLS_TYPEOF(BoundingSphere);
    ASSERT_TRUE(type.IsValid());
    const auto schema = NativeSerializedStreamCodec::BuildTypeTree(type, {"value", false});
    ASSERT_NE(schema, nullptr);
    SerializedReferenceTable references;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(NativeSerializedStreamCodec::EncodeVariant(meta::Variant(source), *schema, references, bytes));
    const auto decoded = NativeSerializedStreamCodec::DecodeVariant(bytes, *schema, type, references);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FLOAT_EQ(decoded->GetValue<BoundingSphere>().position.z, 3.0f);
    EXPECT_FLOAT_EQ(decoded->GetValue<BoundingSphere>().radius, 4.0f);
}

TEST(NativeSerializedStreamTests, CodecRoundTripsAnimationCurveGradientAndExposedReference)
{
    NLS::meta::ReflectionDatabase::Instance();

    AnimationCurve curve;
    curve.keys = {
        AnimationCurveKeyframe {0.0f, 1.0f, 0.25f, 0.5f, 1.0f, 1.0f, 0},
        AnimationCurveKeyframe {2.0f, -3.0f, -0.5f, 0.75f, 0.25f, 0.5f, 1}};
    curve.preWrapMode = 2;
    curve.postWrapMode = 3;

    Gradient gradient;
    gradient.colorKeys = {
        GradientColorKey {Maths::Color(1.0f, 0.0f, 0.0f, 1.0f), 0.0f},
        GradientColorKey {Maths::Color(0.0f, 0.0f, 1.0f, 1.0f), 1.0f}};
    gradient.alphaKeys = {
        GradientAlphaKey {0.25f, 0.0f},
        GradientAlphaKey {0.9f, 1.0f}};
    gradient.mode = 1;

    GameObject object("Exposed Reference Target");
    ExposedReference exposed;
    exposed.defaultValue = PPtr<GameObject>(&object);
    exposed.exposedName = "target";

    const auto roundTrip = [](const auto& source)
    {
        using T = std::decay_t<decltype(source)>;
        T result {};
        const auto type = NLS_TYPEOF(T);
        const auto schema = NativeSerializedStreamCodec::BuildTypeTree(type, {"value", false});
        EXPECT_TRUE(type.IsValid());
        if (schema == nullptr)
        {
            ADD_FAILURE() << "Missing TypeTree for special serialized value.";
            return result;
        }
        SerializedReferenceTable references;
        std::vector<uint8_t> bytes;
        if (!NativeSerializedStreamCodec::EncodeVariant(meta::Variant(source), *schema, references, bytes))
        {
            ADD_FAILURE() << "Special serialized value failed to encode.";
            return result;
        }
        EXPECT_FALSE(bytes.empty());
        const auto decoded = NativeSerializedStreamCodec::DecodeVariant(bytes, *schema, type, references);
        if (!decoded.has_value())
        {
            ADD_FAILURE() << "Special serialized value failed to decode.";
            return result;
        }
        return decoded->template GetValue<T>();
    };

    const auto decodedCurve = roundTrip(curve);
    ASSERT_EQ(decodedCurve.keys.size(), curve.keys.size());
    for (size_t index = 0u; index < curve.keys.size(); ++index)
    {
        EXPECT_FLOAT_EQ(decodedCurve.keys[index].time, curve.keys[index].time);
        EXPECT_FLOAT_EQ(decodedCurve.keys[index].value, curve.keys[index].value);
        EXPECT_FLOAT_EQ(decodedCurve.keys[index].inTangent, curve.keys[index].inTangent);
        EXPECT_FLOAT_EQ(decodedCurve.keys[index].outTangent, curve.keys[index].outTangent);
        EXPECT_EQ(decodedCurve.keys[index].weightedMode, curve.keys[index].weightedMode);
    }
    EXPECT_EQ(decodedCurve.preWrapMode, curve.preWrapMode);
    EXPECT_EQ(decodedCurve.postWrapMode, curve.postWrapMode);
    const auto decodedGradient = roundTrip(gradient);
    ASSERT_EQ(decodedGradient.colorKeys.size(), gradient.colorKeys.size());
    ASSERT_EQ(decodedGradient.alphaKeys.size(), gradient.alphaKeys.size());
    for (size_t index = 0u; index < gradient.colorKeys.size(); ++index)
    {
        EXPECT_FLOAT_EQ(decodedGradient.colorKeys[index].color.r, gradient.colorKeys[index].color.r);
        EXPECT_FLOAT_EQ(decodedGradient.colorKeys[index].color.g, gradient.colorKeys[index].color.g);
        EXPECT_FLOAT_EQ(decodedGradient.colorKeys[index].color.b, gradient.colorKeys[index].color.b);
        EXPECT_FLOAT_EQ(decodedGradient.colorKeys[index].color.a, gradient.colorKeys[index].color.a);
        EXPECT_FLOAT_EQ(decodedGradient.colorKeys[index].time, gradient.colorKeys[index].time);
    }
    for (size_t index = 0u; index < gradient.alphaKeys.size(); ++index)
    {
        EXPECT_FLOAT_EQ(decodedGradient.alphaKeys[index].alpha, gradient.alphaKeys[index].alpha);
        EXPECT_FLOAT_EQ(decodedGradient.alphaKeys[index].time, gradient.alphaKeys[index].time);
    }
    EXPECT_EQ(decodedGradient.mode, gradient.mode);
    const auto decodedExposed = roundTrip(exposed);
    EXPECT_EQ(decodedExposed.exposedName, exposed.exposedName);
    EXPECT_EQ(decodedExposed.defaultValue.Get(), &object);
}
} // namespace NLS::Engine::Serialize::Tests
