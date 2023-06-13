public class BasicSample {

    public void waitForever() throws InterruptedException {
        System.out.print("Waiting forever...");
        for (int i = 0; i < 200; i++) {
            long start = System.currentTimeMillis();
            while (System.currentTimeMillis() - start < 20);
            System.out.print(".");
        }
        System.out.println("done");
    }

    public static void main(String[] args) throws InterruptedException {
        new BasicSample().waitForever();
    }
}