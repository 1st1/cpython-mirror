import types
import unittest
from test import support


class AsyncYieldFrom:
    def __init__(self, obj):
        self.obj = obj

    def __await__(self):
        yield from self.obj


class AsyncYield:
    def __init__(self, value):
        self.value = value

    def __await__(self):
        yield self.value


class AsyncBadSyntaxTest(unittest.TestCase):

    def test_badsyntax_1(self):
        with self.assertRaisesRegex(SyntaxError, 'invalid syntax'):
            import test.badsyntax_async1

    def test_badsyntax_2(self):
        with self.assertRaisesRegex(SyntaxError, 'invalid syntax'):
            import test.badsyntax_async2

    def test_badsyntax_3(self):
        with self.assertRaisesRegex(SyntaxError, 'invalid syntax'):
            import test.badsyntax_async3

    def test_badsyntax_4(self):
        with self.assertRaisesRegex(SyntaxError, 'invalid syntax'):
            import test.badsyntax_async4

    def test_badsyntax_5(self):
        with self.assertRaisesRegex(SyntaxError, 'invalid syntax'):
            import test.badsyntax_async5

    def test_badsyntax_6(self):
        with self.assertRaisesRegex(
            SyntaxError, "'yield' inside async function"):

            import test.badsyntax_async6

    def test_badsyntax_7(self):
        with self.assertRaisesRegex(
            SyntaxError, "'yield from' inside async function"):

            import test.badsyntax_async7


class AsyncFunctionTest(unittest.TestCase):

    def test_func_1(self):
        async def foo():
            return 10

        f = foo()
        self.assertIsInstance(f, types.GeneratorType)
        self.assertTrue(bool(foo.__code__.co_flags & 0x80))
        self.assertTrue(bool(foo.__code__.co_flags & 0x20))
        self.assertTrue(bool(f.gi_code.co_flags & 0x80))
        self.assertTrue(bool(f.gi_code.co_flags & 0x20))
        try:
            next(f)
        except StopIteration as ex:
            self.assertEqual(ex.args[0], 10)
        else:
            self.assertTrue(False)

        def bar(): pass
        self.assertFalse(bool(bar.__code__.co_flags & 0x80))

    def test_func_2(self):
        async def foo():
            raise StopIteration

        with self.assertRaisesRegex(
                RuntimeError, "generator raised StopIteration"):

            next(foo())

    def test_await_1(self):
        async def foo():
            await 1
        with self.assertRaisesRegex(RuntimeError, "object 1 can.t.*await"):
            list(foo())

    def test_await_2(self):
        async def foo():
            await []
        with self.assertRaisesRegex(RuntimeError, "object \[\] can.t.*await"):
            list(foo())

    def test_await_3(self):
        async def foo():
            await AsyncYieldFrom([1, 2, 3])

        self.assertEqual(list(foo()), [1, 2, 3])

    def test_await_4(self):
        async def bar():
            return 42

        async def foo():
            return (await bar())

        try:
            next(foo())
        except StopIteration as ex:
            self.assertEqual(ex.args[0], 42)
        else:
            self.assertFalse(True)

    def test_await_5(self):
        class Awaitable:
            def __await__(self):
                return

        async def foo():
            return (await Awaitable())

        with self.assertRaisesRegex(RuntimeError,
                                    "__await__ returned non-iterator"):

            next(foo())

    def test_await_6(self):
        class Awaitable:
            def __await__(self):
                return iter([52])

        async def foo():
            return (await Awaitable())

        self.assertEqual(next(foo()), 52)

    def test_await_7(self):
        class Awaitable:
            def __await__(self):
                yield 42

        async def foo():
            return (await Awaitable())

        self.assertEqual(next(foo()), 42)

    def test_await_8(self):
        class Awaitable:
            pass

        async def foo():
            return (await Awaitable())

        with self.assertRaisesRegex(
            RuntimeError, "Awaitable object.*used in 'await' expression"):

            self.assertEqual(next(foo()), 42)

    def test_with_1(self):
        class Manager:
            def __init__(self, name):
                self.name = name

            async def __aenter__(self):
                await AsyncYieldFrom(['enter-1-' + self.name,
                                      'enter-2-' + self.name])
                return self

            async def __aexit__(self, *args):
                await AsyncYieldFrom(['exit-1-' + self.name,
                                      'exit-2-' + self.name])

                if self.name == 'B':
                    return True


        async def foo():
            async with Manager("A") as a, Manager("B") as b:
                await AsyncYieldFrom([('managers', a.name, b.name)])
                1/0

        f = foo()
        result = list(f)

        self.assertEqual(
            result, ['enter-1-A', 'enter-2-A', 'enter-1-B', 'enter-2-B',
                     ('managers', 'A', 'B'),
                     'exit-1-B', 'exit-2-B', 'exit-1-A', 'exit-2-A']
        )

        async def foo():
            async with Manager("A") as a, Manager("C") as c:
                await AsyncYieldFrom([('managers', a.name, c.name)])
                1/0

        with self.assertRaises(ZeroDivisionError):
            list(foo())

    def test_with_2(self):
        class CM:
            def __aenter__(self):
                pass

        async def foo():
            async with CM():
                pass

        with self.assertRaisesRegex(AttributeError, '__aexit__'):
            list(foo())

    def test_with_3(self):
        class CM:
            def __aexit__(self):
                pass

        async def foo():
            async with CM():
                pass

        with self.assertRaisesRegex(AttributeError, '__aenter__'):
            list(foo())

    def test_with_4(self):
        class CM:
            def __enter__(self):
                pass

            def __exit__(self):
                pass

        async def foo():
            async with CM():
                pass

        with self.assertRaisesRegex(AttributeError, '__aexit__'):
            list(foo())

    def test_with_5(self):
        # While this test doesn't make a lot of sense,
        # it's a regression test for an early bug with opcodes
        # generation

        class CM:
            async def __aenter__(self):
                return self

            async def __aexit__(self, *exc):
                pass

        async def func():
            async with CM():
                assert (1, ) == 1

        with self.assertRaises(AssertionError):
            next(func())

    def test_with_6(self):
        class CM:
            def __aenter__(self):
                return 123

            def __aexit__(self, *e):
                return 456

        async def foo():
            async with CM():
                pass

        with self.assertRaisesRegex(
            RuntimeError, "object 123 can't be used in 'await' expression"):
            # it's important that __aexit__ wasn't called
            list(foo())

    def test_with_7(self):
        class CM:
            async def __aenter__(self):
                return self

            def __aexit__(self, *e):
                return 456

        async def foo():
            async with CM():
                pass

        with self.assertRaisesRegex(
            RuntimeError, "object 456 can't be used in 'await' expression"):

            list(foo())

    def test_for_1(self):
        aiter_calls = 0

        class AsyncIter:
            def __init__(self):
                self.i = 0

            async def __aiter__(self):
                nonlocal aiter_calls
                aiter_calls += 1
                return self

            async def __anext__(self):
                self.i += 1

                if not (self.i % 10):
                    await AsyncYield(self.i * 10)

                if self.i > 100:
                    raise StopAsyncIteration

                return self.i, self.i


        buffer = []
        async def test1():
            async for i1, i2 in AsyncIter():
                buffer.append(i1 + i2)

        yielded = list(test1())
        # Make sure that __aiter__ was called only once
        self.assertEqual(aiter_calls, 1)
        self.assertEqual(yielded, [i * 100 for i in range(1, 11)])
        self.assertEqual(buffer, [i*2 for i in range(1, 101)])


        buffer = []
        async def test2():
            nonlocal buffer
            async for i in AsyncIter():
                buffer.append(i[0])
                if i[0] == 20:
                    break
            else:
                buffer.append('what?')
            buffer.append('end')

        yielded = list(test2())
        # Make sure that __aiter__ was called only once
        self.assertEqual(aiter_calls, 2)
        self.assertEqual(yielded, [100, 200])
        self.assertEqual(buffer, [i for i in range(1, 21)] + ['end'])


        buffer = []
        async def test3():
            nonlocal buffer
            async for i in AsyncIter():
                if i[0] > 20:
                    continue
                buffer.append(i[0])
            else:
                buffer.append('what?')
            buffer.append('end')

        yielded = list(test3())
        # Make sure that __aiter__ was called only once
        self.assertEqual(aiter_calls, 3)
        self.assertEqual(yielded, [i * 100 for i in range(1, 11)])
        self.assertEqual(buffer, [i for i in range(1, 21)] +
                                 ['what?', 'end'])

    def test_for_2(self):
        async def foo():
            async for i in (1, 2, 3):
                print('never going to happen')

        with self.assertRaisesRegex(
                RuntimeError, "async for' requires an object.*__aiter__"):

            list(foo())

    def test_for_3(self):
        class I:
            def __aiter__(self):
                return self

        async def foo():
            async for i in I():
                print('never going to happen')

        with self.assertRaisesRegex(
                RuntimeError, "async for' received an invalid object.*__aiter"):

            list(foo())

    def test_for_4(self):
        class I:
            async def __aiter__(self):
                return self

            def __anext__(self):
                return ()

        async def foo():
            async for i in I():
                print('never going to happen')

        with self.assertRaisesRegex(
                RuntimeError, "async for' received an invalid object.*__anext"):
            list(foo())

    def test_for_5(self):
        class I:
            async def __aiter__(self):
                return self

            def __anext__(self):
                return 123

        async def foo():
            async for i in I():
                print('never going to happen')

        with self.assertRaisesRegex(
                RuntimeError, "async for' received an invalid object.*__anext"):

            list(foo())


class AsyncAsyncIOCompatTest(unittest.TestCase):

    def test_asyncio_1(self):
        import asyncio

        class MyException(Exception):
            pass

        buffer = []

        class CM:
            async def __aenter__(self):
                buffer.append(1)
                await asyncio.sleep(0.01)
                buffer.append(2)
                return self

            async def __aexit__(self, exc_type, exc_val, exc_tb):
                await asyncio.sleep(0.01)
                buffer.append(exc_type.__name__)

        async def f():
            async with CM() as c:
                await asyncio.sleep(0.01)
                raise MyException
            buffer.append('unreachable')

        loop = asyncio.get_event_loop()
        try:
            loop.run_until_complete(f())
        except MyException:
            pass
        finally:
            loop.close()

        self.assertEqual(buffer, [1, 2, 'MyException'])


class SysSetAsyncWrapperTest(unittest.TestCase):

    def test_set_wrapper_1(self):
        import sys

        async def foo():
            return 'spam'

        wrapped = None
        def wrap(gen):
            nonlocal wrapped
            wrapped = gen
            return gen

        sys.set_async_wrapper(wrap)
        try:
            f = foo()
            self.assertTrue(wrapped)

            with self.assertRaisesRegexp(StopIteration, 'spam'):
                next(f)
        finally:
            sys.set_async_wrapper(None)

        wrapped = None
        f = foo()
        self.assertFalse(wrapped)


def test_main():
    support.run_unittest(AsyncBadSyntaxTest,
                         AsyncFunctionTest,
                         AsyncAsyncIOCompatTest,
                         SysSetAsyncWrapperTest)


if __name__=="__main__":
    test_main()
