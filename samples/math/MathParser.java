package math;

/**
 * An arithmetic expression parser, evaluator and generator, used as an example program.
 * <p>
 * Its main loop is purposefully built to trigger errors in the sampling.
 */
public class MathParser {

    public static void run(int seed, int rounds, int size) {
        ASCII a = new ASCII().init();
        int[] input = new int[2];
        input[0] = a._0 + 5;
        input[1] = a.exclm;
        Lexer lexer = new Lexer().init(input, 2);
        System.out.println(new Parser().init(lexer).parse().eval());
        int z = 0;
        LehmerRandom lehmerRand = new LehmerRandom().init(seed);
        int s = 0;
        while (z < rounds) {
            MathGenerator gen = new MathGenerator().generate(size, lehmerRand);
            int j = 0;
            s += gen.size;
            for (int i = 0; i < 4; i++) {
                ASTNode result = new Parser().init(new Lexer().init(gen.arr, gen.size - i * 100)).parse();
                s += result == null ? 1 : result.number;
            }
            z = z + 1;
        }
        System.out.println(s);
    }

    public static void main(String[] args) {
        run(100, 1000000, 1000);
    }
}

/* Adapted from: https://en.wikipedia.org/wiki/Lehmer_random_number_generator */
class LehmerRandom {
    public int M; /* 2^31 - 1 (A large prime number) */
    public int A;      /* Prime root of M, passes statistical tests and produces a full cycle */
    public int Q; /* M / A (To avoid overflow on A * seed) */
    public int R; /* M % A (To avoid overflow on A * seed) */
    public int seed;

    public LehmerRandom init(int seed) {
        this.M = 2147483647;
        this.A = 16807;
        this.Q = 127773;
        this.R = 2836;
        this.seed = seed;
        return this;
    }

    public LehmerRandom initWithDefault() {
        return init(2147480677);
    }

    public int random() {
        int hi = seed / Q;
        int lo = seed % Q;
        int test = A * lo - R * hi;
        if (test <= 0)
            test = test + M;
        seed = test;
        return test;
    }

    public int next() {
        return random();
    }

    /**
     * @param min minimum number
     * @param max exclusive range end
     */
    public int nextRange(int min, int max) {
        return next() % (max - min) + min;
    }

    public int[] intArray(int size, int min, int maxEx) {
        int[] arr = new int[size];
        int i = 0;
        while (i < size) {
            arr[i] = nextRange(min, maxEx);
            i = i + 1;
        }
        return arr;
    }

    public boolean nextBoolean() {
        return next() % 2 == 0;
    }

    public boolean[] booleanArray(int size) {
        boolean[] arr = new boolean[size];
        int i = 0;
        while (i < size) {
            arr[i] = nextBoolean();
            i = i + 1;
        }
        return arr;
    }

    public void shuffleIntArray(int[] arr, int size) {
        int i = size - 1;
        while (i > 0) {
            int index = nextRange(0, i + 1);
            int a = arr[index];
            arr[index] = arr[i];
            arr[i] = a;
            i = i - 1;
        }
    }
}

class Math {
    public int pow(int v, int exp) {
        if (exp < 0) {
            return 1 / pow(v, -exp);
        } else if (exp == 0) {
            return 1;
        } else {
            int ret = 1;
            while (exp > 0) {
                if (exp % 2 == 0) {
                    v = v * v;
                    exp = exp / 2;
                } else {
                    ret = ret * v;
                    exp = exp - 1;
                }
            }
            return ret;
        }
    }

    public int factorial(int val) {
        int ret = 1;
        int sign = signum(val);
        if (val < 0) {
            val = -val;
        }
        if (val == 0) {
            return 1;
        }
        while (val > 0) {
            ret = ret * val;
            val = val - 1;
        }
        return ret * sign;
    }

    public int min(int s, int t) {
        if (s < t) {
            return s;
        }
        return t;
    }

    public int max(int s, int t) {
        if (s > t) {
            return s;
        }
        return t;
    }

    public int lengthInChars(int num) {
        int len = 1;
        if (num < 0) {
            len = len + 1;
            num = -num;
        }
        while (num > 10) {
            num = num / 10;
            len = len + 1;
        }
        return len;
    }

    public int signum(int num) {
        return Integer.compare(num, 0);
    }

    public int abs(int num) {
        if (num < 0) {
            return -num;
        }
        return num;
    }

}

class BooleanUtils {

    public int toInt(boolean value) {
        if (value) {
            return 1;
        } else {
            return 0;
        }
    }

    public BooleanUtils println(boolean value) {
        System.out.println(toInt(value));
        return this;
    }
}

class ArrayUtils {
    public ArrayUtils printIntArray(int[] arr, int size) {
        int i = 0;
        while (i < size) {
            System.out.println(arr[i]);
            i = i + 1;
        }
        return this;
    }

    public ArrayUtils printBooleanArray(boolean[] arr, int size) {
        int i = 0;
        while (i < size) {
            new BooleanUtils().println(arr[i]);
            i = i + 1;
        }
        return this;
    }

    public int[] copyIntArray(int[] src, int start, int length) {
        int[] ret = new int[length];
        int i = start;
        while (i < start + length) {
            ret[i - start] = src[i];
            i = i + 1;
        }
        return ret;
    }

    public int toInt(int[] chars, int size) {
        int val = 0;
        size = size - 1;
        while (size >= 0) {
            val = val * 10 + chars[size] - new ASCII().init()._0;
            size = size - 1;
        }
        return val;
    }

    public int[] qsort(int[] a, int size) {
        _qsort(a, 0, size - 1);
        return a;
    }

    /**
     * Adapted from <a href="http://stackoverflow.com/a/29610583">...</a>
     */
    public void _qsort(int[] a, int left, int right) {
        if (right > left) {
            int i = left;
            int j = right;
            int tmp;

            int v = a[right];
            boolean breakLoop = false;
            while (!breakLoop) {
                while (a[i] < v) i = i + 1;
                while (a[j] > v) j = j - 1;

                if (i <= j) {
                    tmp = a[i];
                    a[i] = a[j];
                    a[j] = tmp;
                    i = i + 1;
                    j = j - 1;
                }
                if (i > j) {
                    breakLoop = true;
                }
            }
            if (left < j) _qsort(a, left, j);

            if (i < right) _qsort(a, i, right);
        }
    }

    public int[] _marr;

    public int[] msort(int[] arr, int size) {
        this._marr = arr;
        _msort(0, size - 1);
        return _marr;
    }

    public void _msort(int low, int high) {
        if (low < high) {
            int mid = ((low + high) / 2);
            _msort(low, mid);
            _msort(mid + 1, high);
            _msort_merge(low, mid, high);
        }
    }

    /*
    Adapted from http://stackoverflow.com/a/20039399
    */
    public void _msort_merge(int low, int mid, int high) {
        int[] temp = new int[high - low + 1];
        int left = low;
        int right = mid + 1;
        int index = 0;

        while (left <= mid && right <= high) {
            if (_marr[left] < this._marr[right]) {
                temp[index] = _marr[left];
                left = left + 1;
            } else {
                temp[index] = _marr[right];
                right = right + 1;
            }
            index = index + 1;
        }

        while (left <= mid || right <= high) {
            if (left <= mid) {
                temp[index] = _marr[left];
                left = left + 1;
                index = index + 1;
            } else if (right <= high) {
                temp[index] = _marr[right];
                right = right + 1;
                index = index + 1;
            }
        }
        int i = 0;
        while (i < high - low + 1) {
            _marr[low + i] = temp[i];
            i = i + 1;
        }
    }
}

class ASCII {
    public int ws;
    public int _0;
    public int _9;
    public int lparen;
    public int rparen;
    public int plus;
    public int minus;
    public int star;
    /**
     * /
     */
    public int slash;
    /**
     * !
     */
    public int exclm;
    /**
     * %
     */
    public int mod;

    public ASCII init() {
        this.ws = 32;
        lparen = 40;
        rparen = 41;
        this._0 = 48;
        this._9 = 57;
        star = 42;
        plus = 43;
        minus = 45;
        slash = 47;
        exclm = 33;
        mod = 37;
        return this;
    }
}

class Terminals {
    public Terminal number;

    public Terminal plus;
    public Terminal minus;
    public Terminal mul;
    public Terminal div;
    public Terminal eof;
    public Terminal pow;
    public Terminal lparen;
    public Terminal rparen;
    public Terminal factorial;
    public Terminal modulo;

    public Terminals init() {
        this.plus = new Terminal().init(0, 6, true, true, false);
        this.minus = new Terminal().init(3, 6, true, true, true);
        this.mul = new Terminal().init(1, 7, true, true, false);
        this.div = new Terminal().init(2, 7, true, true, false);
        this.modulo = new Terminal().init(9, 7, true, true, false);
        this.pow = new Terminal().init(4, 20, false, true, false);
        this.eof = new Terminal().initNonOp(2);
        this.lparen = new Terminal().initNonOp(5);
        this.rparen = new Terminal().initNonOp(6);
        this.number = new Terminal().initNonOp(7);
        this.factorial = new Terminal().init(8, -1, false, false, true);
        return this;
    }


}

class Terminal {
    public int id;
    public int precedence;
    public boolean leftAssociative;
    public boolean binary;
    public boolean unary;

    public Terminal init(int id, int precedence, boolean leftAssociative, boolean binary, boolean unary) {
        this.id = id;
        this.precedence = precedence;
        this.leftAssociative = leftAssociative;
        this.binary = binary;
        this.unary = unary;
        return this;
    }

    public Terminal initNonOp(int id) {
        return init(id, -1, false, false, false);
    }

    public boolean isOperator() {
        return precedence != -1;
    }

    public boolean equals(Terminal other) {
        return id == other.id;
    }
}

class Token {

    public Terminal type;
    public int[] chars;
    public int charsSize;

    public Token init(Terminal type, int[] chars, int charsSize) {
        this.type = type;
        this.chars = chars;
        this.charsSize = charsSize;
        return this;
    }

    public boolean isTerminal(Terminal terminal) {
        return type.id == terminal.id;
    }
}

class Lexer {
    public int _pos;
    public int size;
    public int[] chars;
    public ASCII ascii;
    public Terminals t;

    public Lexer init(int[] chars, int size) {
        this._pos = 0;
        this.size = size;
        this.chars = chars;
        this.ascii = new ASCII().init();
        this.t = new Terminals().init();
        return this;
    }

    public void ignoreWS() {
        while (_pos < size && chars[_pos] == ascii.ws) _pos = _pos + 1;
    }

    public Token nextToken() {
        ignoreWS();
        if (_pos >= size) {
            return new Token().init(t.eof, new int[0], 0);
        }
        if (isDigit(_cur())) {
            return parseDigit();
        }
        int[] _curArr = new int[1];
        _curArr[0] = _cur();
        Terminal terminal = null;
        if (_cur() == ascii.lparen) {
            terminal = t.lparen;
        } else if (_cur() == ascii.rparen) {
            terminal = t.rparen;
        } else if (_cur() == ascii.plus) {
            terminal = t.plus;
        } else if (_cur() == ascii.minus) {
            terminal = t.minus;
        } else if (_cur() == ascii.slash) {
            terminal = t.div;
        } else if (_cur() == ascii.exclm) {
            terminal = t.factorial;
        } else if (_cur() == ascii.mod) {
            terminal = t.modulo;
        } else if (_cur() == -1) {
            terminal = t.eof;
        }
        if (terminal != null) {
            _next();
            return new Token().init(terminal, _curArr, 1);
        } else {
            if (_cur() == ascii.star) {
                _next();
                return parseStar();
            }
        }
        return null;
    }

    public Token parseDigit() {
        int beginPos = _pos;
        while (isDigit(_next())) ;
        int size = _pos - beginPos;
        int[] chars = new ArrayUtils().copyIntArray(this.chars, _pos - size, size);
        return new Token().init(t.number, chars, size);
    }

    public Token parseStar() {
        if (_cur() == ascii.star) {
            _next();
            int[] arr = new int[2];
            arr[0] = ascii.star;
            arr[1] = ascii.star;
            return new Token().init(t.pow, arr, 2);
        } else {
            int[] arr = new int[2];
            arr[0] = ascii.star;
            return new Token().init(t.mul, arr, 1);
        }
    }

    public int _cur() {
        if (_pos >= size) {
            return -1;
        }
        return chars[_pos];
    }

    public int _next() {
        if (_pos >= size) {
            return -1;
        }
        _pos = _pos + 1;
        return _cur();
    }

    public boolean isDigit(int c) {
        return ascii._0 <= c && ascii._9 >= c;
    }
}

class ASTNode {
    public boolean isBinOp;
    public boolean isUnaryOp;
    public boolean isNumber;

    public int number;
    public Terminal op;
    public ASTNode left;
    public ASTNode right;
    public ASTNode subNode;

    public Terminals t;

    public ASTNode initBinOp(Terminal op, ASTNode left, ASTNode right) {
        _init();
        isBinOp = true;
        this.op = op;
        this.left = left;
        this.right = right;
        return this;
    }

    public ASTNode initUnOp(Terminal op, ASTNode subNode) {
        _init();
        isUnaryOp = true;
        this.subNode = subNode;
        this.op = op;
        return this;
    }

    public ASTNode initNumber(int number) {
        _init();
        isNumber = true;
        this.number = number;
        return this;
    }

    public void _init() {
        isBinOp = false;
        isUnaryOp = false;
        isNumber = false;
        t = new Terminals().init();
    }

    public int eval() {
        if (isNumber) {
            return number;
        }
        if (isBinOp) {
            if (op.equals(t.plus)) {
                return left.eval() + right.eval();
            }
            if (op.equals(t.minus)) {
                return left.eval() - right.eval();
            }
            if (op.equals(t.mul)) {
                return left.eval() * right.eval();
            }
            if (op.equals(t.modulo)) {
                int rhs = right.eval();
                if (rhs == 0) {
                    return 0;
                }
                return left.eval() % rhs;
            }
            if (op.equals(t.div)) {
                int rhs = right.eval();
                if (rhs == 0) {
                    return 0;
                }
                return left.eval() / rhs;
            }
            if (op.equals(t.pow)) {
                return new Math().pow(left.eval(), right.eval());
            }
        }
        if (isUnaryOp) {
            if (op.equals(t.minus)) {
                return -subNode.eval();
            }
            if (op.equals(t.factorial)) {
                return new Math().factorial(subNode.eval());
            }
        }
        return 0;
    }
}

class Parser {

    public Lexer lexer;
    public Token _cur;
    public Terminals t;

    public Parser init(Lexer lexer) {
        this.lexer = lexer;
        _cur = lexer.nextToken();
        t = new Terminals().init();
        return this;
    }

    public ASTNode parse() {
        return _parseExpressionWithPrecedenceClimbing(0);
    }

    public ASTNode _parseExpressionWithPrecedenceClimbing(int minPrecedence) {
        ASTNode result = _parseUnaryExpression();
        while (_isCurrentTokenBinaryOperator()
                && _isOperatorPrecedenceGreaterOrEqualThan(minPrecedence)) {
            int precedence = _cur.type.precedence;
            Terminal op = _cur.type;
            if (_cur.type.leftAssociative) {
                precedence = precedence + 1;
            }
            _next();
            ASTNode rhs = _parseExpressionWithPrecedenceClimbing(precedence);
            result = new ASTNode().initBinOp(op, result, rhs);
        }
        return result;
    }

    public ASTNode _parseUnaryExpression() {
        Terminal op = _cur.type;
        ASTNode node = null;
        if (_cur.isTerminal(t.minus)) {
            _next();
            node = new ASTNode().initUnOp(op, _parseUnaryExpression());
        } else {
            node = _parsePrimary();
        }
        while (_cur.isTerminal(t.factorial)) {
            node = new ASTNode().initUnOp(_cur.type, node);
            _next();
        }
        return node;
    }

    public ASTNode _parsePrimary() {
        ASTNode ret = null;
        if (_cur.isTerminal(t.lparen)) {
            _next();
            ret = parse();
            if (_cur.isTerminal(t.rparen)) {
                _next();
            }
        } else if (_cur.isTerminal(t.number)) {
            ret = new ASTNode().initNumber(new ArrayUtils().toInt(_cur.chars, _cur.charsSize));
            _next();
        }
        return ret;
    }

    public boolean _isCurrentTokenBinaryOperator() {
        return _cur.type.binary;
    }

    public boolean _isOperatorPrecedenceGreaterOrEqualThan(int minPrecedence) {
        return _cur.type.precedence >= minPrecedence;
    }

    public Token _next() {
        _cur = lexer.nextToken();
        return _cur;
    }
}

class MathGenerator {
    public int curIndex;
    public int maxSize;
    public int[] arr;
    public int size;
    public LehmerRandom random;
    public Math math;
    public ASCII ascii;

    public MathGenerator generate(int maxSize, LehmerRandom random) {
        curIndex = 0;
        this.maxSize = maxSize;
        arr = new int[maxSize];
        this.random = random;
        this.math = new Math();
        this.ascii = new ASCII().init();
        _genExpression();
        size = curIndex;
        return this;
    }

    public void _genExpression() {
        if (!_areCharsAv()) {
            _genNumber();
        } else {
            if (random.nextRange(0, 10) <= 2) {
                _genUnary();
                arr[curIndex] = ascii.star;
                arr[curIndex + 1] = ascii.star;
                arr[curIndex + 2] = ascii._0 + random.nextRange(2, 10);
                curIndex = curIndex + 3;
            } else {
                int[] ops = new int[5];
                ops[0] = ascii.plus;
                ops[1] = ascii.minus;
                ops[2] = ascii.star;
                ops[3] = ascii.slash;
                ops[4] = ascii.mod;
                int opIndex = 0;
                if (random.nextRange(0, 10) == 0) {
                    opIndex = 3 + random.nextRange(0, 2);
                } else {
                    opIndex = random.nextRange(0, 3);
                }
                _genUnary();
                arr[curIndex] = ops[opIndex];
                curIndex = curIndex + 1;
                _genUnary();
            }
        }
    }

    public void _genUnary() {
        if (!_areCharsAv()) {
            _genNumber();
        } else {
            if (random.nextRange(0, 10) <= 2) {
                arr[curIndex] = ascii.minus;
                curIndex = curIndex + 1;
                _genUnary();
            } else if (random.nextRange(0, 20) <= 1) {
                _genUnary();
                arr[curIndex] = ascii.exclm;
                curIndex = curIndex + 1;
            } else {
                _genPrimary();
            }
        }
    }

    public void _genPrimary() {
        if (!_areCharsAv()) {
            _genNumber();
        } else if (random.nextRange(0, 10) <= 4) {
            arr[curIndex] = ascii.lparen;
            curIndex = curIndex + 1;
            _genExpression();
            arr[curIndex] = ascii.rparen;
            curIndex = curIndex + 1;
        } else {
            _genNumber();
        }
    }

    public void _genNumber() {
        int av = math.min(math.max(1, _avChars()), 5);
        int num = random.nextRange(-math.pow(10, av - 1), math.pow(10, av));
        if (num < 0) {
            arr[curIndex] = ascii.minus;
            num = -num;
            curIndex = curIndex + 1;
        }
        int i = 0;
        int len = math.lengthInChars(num);

        while (i < len) {
            arr[curIndex + len - i - 1] = ascii._0 + (num % 10);
            num = num / 10;
            i = i + 1;
        }
        curIndex = curIndex + len;
    }

    public int _avChars() {
        return math.max(0, maxSize / 3 - curIndex);
    }

    public boolean _areCharsAv() {
        return _avChars() > 3;
    }
}