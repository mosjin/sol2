#define SOL_CHECK_ARGUMENTS 1
#define SOL_ENABLE_INTEROP 1

#include <sol.hpp>
#include <catch.hpp>

#include <iostream>
#include <string>

TEST_CASE("filters/self", "ensure we return a direct reference to the lua userdata rather than creating a new one") {
	struct vec2 {
		float x = 20.f;
		float y = 20.f;

		vec2& normalize() {
			float len2 = x * x + y * y;
			if (len2 != 0) {
				float len = sqrtf(len2);
				x /= len;
				y /= len;
			}
			return *this;
		}

		~vec2() {
			x = std::numeric_limits<float>::lowest();
			y = std::numeric_limits<float>::lowest();
		}
	};

	sol::state lua;
	lua.open_libraries(sol::lib::base);

	lua.new_usertype<vec2>("vec2",
		"x", &vec2::x,
		"y", &vec2::y,
		"normalize", sol::filters(&vec2::normalize, sol::returns_self()));
	REQUIRE_NOTHROW([&]() {
		lua.safe_script(R"(
v1 = vec2.new()
print('v1:', v1.x, v1.y)
v2 = v1:normalize()
print('v1:', v1.x, v1.y)
print('v2:', v2.x, v2.y)
print(v1, v2)
assert(rawequal(v1, v2))
v1 = nil
collectgarbage()
print(v2) -- v2 points to same, is not destroyed
		)");
	}());
}

TEST_CASE("filters/self_dependency", "ensure we can keep a userdata instance alive by attaching it to the lifetime of another userdata") {
	struct dep;
	struct gc_test;
	static std::vector<dep*> deps_destroyed;
	static std::vector<gc_test*> gc_tests_destroyed;

	struct dep {
		int value = 20;
		~dep() {
			std::cout << "\t"
					<< "[C++] ~dep" << std::endl;
			value = std::numeric_limits<int>::max();
			deps_destroyed.push_back(this);
		}
	};

	struct gc_test {

		dep d;

		~gc_test() {
			std::cout << "\t"
					<< "[C++] ~gc_test" << std::endl;
			gc_tests_destroyed.push_back(this);
		}
	};

	sol::state lua;
	lua.open_libraries(sol::lib::base);

	lua.new_usertype<dep>("dep",
		"value", &dep::value,
		sol::meta_function::to_string, [](dep& d) {
			return "{ " + std::to_string(d.value) + " }";
		});
	lua.new_usertype<gc_test>("gc_test",
		"d", sol::filters(&gc_test::d, sol::self_dependency()),
		sol::meta_function::to_string, [](gc_test& g) {
			return "{ d: { " + std::to_string(g.d.value) + " } }";
		});

	lua.safe_script(R"(
g = gc_test.new()
d = g.d
print("new gc_test, d = g.d")
print("", g)
)");
	REQUIRE(deps_destroyed.empty());
	REQUIRE(gc_tests_destroyed.empty());

	gc_test* g = lua["g"];
	dep* d = lua["d"];

	lua.safe_script(R"(
print("g = nil, collectgarbage")
g = nil
collectgarbage()
print("", d)
)");
	REQUIRE(deps_destroyed.empty());
	REQUIRE(gc_tests_destroyed.empty());

	lua.safe_script(R"(
print("d = nil, collectgarbage")
d = nil
collectgarbage()
)");

	REQUIRE(deps_destroyed.size() == 1);
	REQUIRE(gc_tests_destroyed.size() == 1);
	REQUIRE(deps_destroyed[0] == d);
	REQUIRE(gc_tests_destroyed[0] == g);
}

TEST_CASE("filters/stack_dependencies", "ensure we can take dependencies even to arguments pushed on the stack") {
	struct holder;
	struct depends_on_reference;
	struct composition_related;
	static std::vector<composition_related*> composition_relateds_destroyed;
	static std::vector<holder*> holders_destroyed;
	static std::vector<depends_on_reference*> depends_on_references_destroyed;

	struct composition_related {
		std::string text = "bark";

		~composition_related() {
			std::cout << "[C++] ~composition_related" << std::endl;
			text = "";
			composition_relateds_destroyed.push_back(this);
		}
	};

	struct holder {
		int value = 20;
		~holder() {
			std::cout << "[C++] ~holder" << std::endl;
			value = std::numeric_limits<int>::max();
			holders_destroyed.push_back(this);
		}
	};

	struct depends_on_reference {

		std::reference_wrapper<holder> href;
		composition_related comp;

		depends_on_reference(holder& h)
		: href(h) {
		}

		~depends_on_reference() {
			std::cout << "[C++] ~depends_on_reference" << std::endl;
			depends_on_references_destroyed.push_back(this);
		}
	};

	sol::state lua;
	lua.open_libraries(sol::lib::base);

	lua.new_usertype<holder>("holder",
		"value", &holder::value);
	lua.new_usertype<depends_on_reference>("depends_on_reference",
		"new", sol::filters(sol::constructors<depends_on_reference(holder&)>(), sol::stack_dependencies(-1, 1)),
		"comp", &depends_on_reference::comp);

	lua.safe_script(R"(
h = holder.new()
dor = depends_on_reference.new(h)
c = dor.comp
)");
	REQUIRE(composition_relateds_destroyed.empty());
	REQUIRE(holders_destroyed.empty());
	REQUIRE(depends_on_references_destroyed.empty());

	holder* h = lua["h"];
	composition_related* c = lua["c"];
	depends_on_reference* dor = lua["dor"];

	REQUIRE(h == &dor->href.get());
	REQUIRE(c == &dor->comp);

	lua.safe_script(R"(
h = nil
collectgarbage()
)");
	REQUIRE(composition_relateds_destroyed.empty());
	REQUIRE(holders_destroyed.empty());
	REQUIRE(depends_on_references_destroyed.empty());

	lua.safe_script(R"(
c = nil
collectgarbage()
)");

	REQUIRE(composition_relateds_destroyed.empty());
	REQUIRE(holders_destroyed.empty());
	REQUIRE(depends_on_references_destroyed.empty());

	lua.safe_script(R"(
dor = nil
collectgarbage()
)");

	REQUIRE(composition_relateds_destroyed.size() == 1);
	REQUIRE(holders_destroyed.size() == 1);
	REQUIRE(depends_on_references_destroyed.size() == 1);
	REQUIRE(composition_relateds_destroyed[0] == c);
	REQUIRE(holders_destroyed[0] == h);
	REQUIRE(depends_on_references_destroyed[0] == dor);
}

int always_return_24(lua_State* L, int) {
	return sol::stack::push(L, 24);
}

TEST_CASE("filters/custom", "ensure we can return dependencies on multiple things in the stack") {

	sol::state lua;
	lua.set_function("f", sol::filters([]() { return std::string("hi there"); }, always_return_24));

	int value = lua["f"]();
	REQUIRE(value == 24);
}
