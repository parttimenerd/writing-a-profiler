public class BasicSample {

    public void waitForever() throws InterruptedException {
        System.out.print("Waiting forever...");
        for (int i = 0; i < 150; i++) {
            fib(i / 5);
        }
        System.out.println("done");
    }

    public static void main(String[] args) throws InterruptedException {
        new BasicSample().waitForever();
    }

    public int fib(int n) {
        return n < 2 ? n : fib(n - 1) + fib(n - 2);
    }
}