// SmallFunction.h

#pragma once

#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace myengine::jobs
{
	// Only declared: SmallFunction is usable with a function signature, e.g. SmallFunction<void(int), 64>
	template <typename Signature, std::size_t Capacity>
	class SmallFunction;

	// Fixed-size callable wrapper without heap allocations
	// The functor is stored inside the object; a too big capture is a compile error
	template <typename Return, typename... Args, std::size_t Capacity>
	class SmallFunction<Return(Args...), Capacity>
	{
	public:
		SmallFunction() noexcept = default;

		template <typename Functor, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Functor>, SmallFunction>>>
		SmallFunction(Functor&& functor)
		{
			using Stored = std::decay_t<Functor>;
			static_assert(sizeof(Stored) <= Capacity, "Job captures too much data: capture a pointer to a struct instead");
			static_assert(alignof(Stored) <= alignof(std::max_align_t), "Over-aligned functors are not supported");
			static_assert(std::is_invocable_r_v<Return, Stored&, Args...>, "Functor does not match the signature");
			static_assert(std::is_copy_constructible_v<Stored>, "Functor must be copyable: Dispatch copies the job into every group");
			static_assert(std::is_nothrow_move_constructible_v<Stored>, "Functor must be nothrow movable");

			new(storage_) Stored(std::forward<Functor>(functor));
			invoke_ = &InvokeImpl<Stored>;
			manage_ = &ManageImpl<Stored>;
		}

		SmallFunction(const SmallFunction& other)
		{
			CopyFrom(other);
		}

		SmallFunction(SmallFunction&& other) noexcept
		{
			MoveFrom(other);
		}

		SmallFunction& operator=(const SmallFunction& other)
		{
			if (this != &other)
			{
				Reset();
				CopyFrom(other);
			}
			return *this;
		}

		SmallFunction& operator=(SmallFunction&& other) noexcept
		{
			if (this != &other)
			{
				Reset();
				MoveFrom(other);
			}
			return *this;
		}

		~SmallFunction()
		{
			Reset();
		}

		void Reset() noexcept
		{
			if (manage_ != nullptr)
			{
				manage_(Operation::Destroy, storage_, nullptr);
			}
			invoke_ = nullptr;
			manage_ = nullptr;
		}

		Return operator()(Args... args)
		{
			assert(invoke_ != nullptr && "Calling an empty SmallFunction");
			return invoke_(storage_, std::forward<Args>(args)...);
		}

		explicit operator bool() const noexcept
		{
			return invoke_ != nullptr;
		}

	private:
		enum class Operation
		{
			Copy, // construct self from source
			Move, // construct self from source, then destroy source
			Destroy // destroy self
		};

		using InvokeFn = Return(*)(void* storage, Args... args);
		using ManageFn = void(*)(Operation operation, void* self, const void* source);

		template <typename Stored>
		static Return InvokeImpl(void* storage, Args... args)
		{
			return (*static_cast<Stored*>(storage))(std::forward<Args>(args)...);
		}

		template <typename Stored>
		static void ManageImpl(Operation operation, void* self, const void* source)
		{
			switch (operation)
			{
			case Operation::Copy:
				new(self) Stored(*static_cast<const Stored*>(source));
				break;
			case Operation::Move:
			{
				// Move source is never really const: only CopyFrom passes a truly const object
				auto* movedFrom = static_cast<Stored*>(const_cast<void*>(source));
				new(self) Stored(std::move(*movedFrom));
				movedFrom->~Stored();
				break;
			}
			case Operation::Destroy:
				static_cast<Stored*>(self)->~Stored();
				break;
			}
		}

		void CopyFrom(const SmallFunction& other)
		{
			if (other.manage_ == nullptr)
			{
				return;
			}
			other.manage_(Operation::Copy, storage_, other.storage_);
			invoke_ = other.invoke_;
			manage_ = other.manage_;
		}

		void MoveFrom(SmallFunction& other) noexcept
		{
			if (other.manage_ == nullptr)
			{
				return;
			}
			other.manage_(Operation::Move, storage_, other.storage_);
			invoke_ = other.invoke_;
			manage_ = other.manage_;
			other.invoke_ = nullptr;
			other.manage_ = nullptr;
		}

		alignas(std::max_align_t) unsigned char storage_[Capacity];
		InvokeFn invoke_ = nullptr;
		ManageFn manage_ = nullptr;
	};
}