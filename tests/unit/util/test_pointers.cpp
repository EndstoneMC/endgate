#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "endgate/object.h"
#include "endgate/util/pointers.h"

namespace endgate {
namespace {

class Animal : public Object {
public:
    [[nodiscard]] bool isInstanceOf(const std::type_info &target) const override
    {
        return target == typeid(Animal) || Object::isInstanceOf(target);
    }
};

class Dog : public Animal {
public:
    [[nodiscard]] bool isInstanceOf(const std::type_info &target) const override
    {
        return target == typeid(Dog) || Animal::isInstanceOf(target);
    }
};

TEST(Pointers, NotNullConvertsToBaseAndNullable)
{
    const NotNull<Dog> dog = std::make_shared<Dog>();
    const NotNull<Animal> animal = dog;
    const Nullable<Animal> nullable = animal;
    EXPECT_TRUE(nullable);
    EXPECT_EQ(animal, nullable);
}

TEST(Pointers, AsNarrowsAndKeepsOwnership)
{
    const NotNull<Animal> animal = std::make_shared<Dog>();
    EXPECT_TRUE(animal.is<Dog>());
    const Nullable<Dog> dog = animal.as<Dog>();
    ASSERT_TRUE(dog);
    EXPECT_EQ(animal.get().use_count(), 2);
    EXPECT_FALSE(NotNull<Animal>(std::make_shared<Animal>()).is<Dog>());
}

TEST(Pointers, NullableHoldsNothing)
{
    const Nullable<Animal> empty;
    EXPECT_FALSE(empty);
    EXPECT_FALSE(empty.is<Dog>());
    EXPECT_FALSE(empty.as<Dog>());
    EXPECT_THROW(empty.value(), std::runtime_error);
}

}  // namespace
}  // namespace endgate
