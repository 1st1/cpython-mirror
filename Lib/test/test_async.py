import unittest
from test import support


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


class AsyncFunctionTest(unittest.TestCase):
    def test_func_1(self):
        async def foo():
            return 10
        self.assertTrue(bool(foo.__code__.co_flags & 0x80))
        self.assertTrue(bool(foo.__code__.co_flags & 0x20))
        self.assertTrue(bool(foo().gi_code.co_flags & 0x80))
        self.assertTrue(bool(foo().gi_code.co_flags & 0x20))
        try:
            next(foo())
        except StopIteration as ex:
            self.assertEqual(ex.args[0], 10)
        else:
            self.assertTrue(False)

    def test_with_1(self):
        class Manager:
            def __init__(self, name):
                self.name = name

            async def __aenter__(self):
                yield 'enter-1-' + self.name # xxx
                yield 'enter-2-' + self.name # xxx

                return self

            async def __aexit__(self, *args):
                yield 'exit-1-' + self.name # xxx
                yield 'exit-2-' + self.name # xxx

                if self.name == 'B':
                    return True


        async def foo():
            async with Manager("A") as a, Manager("B") as b:
                yield ('managers', a.name, b.name)
                1/0

        f = foo()
        result = list(f)

        self.assertEqual(
            result, ['enter-1-A', 'enter-2-A', 'enter-1-B', 'enter-2-B',
                     ('managers', 'A', 'B'),
                     'exit-1-B', 'exit-2-B', 'exit-1-A', 'exit-2-A']
        )


def test_main():
    support.run_unittest(AsyncBadSyntaxTest, AsyncFunctionTest)


if __name__=="__main__":
    test_main()
