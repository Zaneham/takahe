-- smoke.vhd -- Basic VHDL for lexer testing
-- If the takahe can tokenise this, it can tokenise VHDL.
-- The US DoD would be proud. Or at least not actively disappointed.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity counter is
    generic (
        WIDTH : integer := 8
    );
    port (
        clk   : in  std_logic;
        rst_n : in  std_logic;
        en    : in  std_logic;
        count : out std_logic_vector(WIDTH-1 downto 0)
    );
end entity counter;

architecture rtl of counter is
    signal cnt : unsigned(WIDTH-1 downto 0);
begin
    process(clk, rst_n)
    begin
        if rst_n = '0' then
            cnt <= (others => '0');
        elsif rising_edge(clk) then
            if en = '1' then
                cnt <= cnt + 1;
            end if;
        end if;
    end process;

    count <= std_logic_vector(cnt);
end architecture rtl;

-- A simple ALU to test case statements
entity alu is
    port (
        a      : in  std_logic_vector(7 downto 0);
        b      : in  std_logic_vector(7 downto 0);
        op     : in  std_logic_vector(2 downto 0);
        result : out std_logic_vector(7 downto 0);
        zero   : out std_logic
    );
end entity alu;

architecture rtl of alu is
    signal r : unsigned(7 downto 0);
begin
    process(a, b, op)
    begin
        case op is
            when "000" =>
                r <= unsigned(a) + unsigned(b);
            when "001" =>
                r <= unsigned(a) - unsigned(b);
            when "010" =>
                r <= unsigned(a) and unsigned(b);
            when "011" =>
                r <= unsigned(a) or unsigned(b);
            when "100" =>
                r <= not unsigned(a);
            when others =>
                r <= unsigned(a);
        end case;
    end process;

    result <= std_logic_vector(r);
    zero <= '1' when r = X"00" else '0';
end architecture rtl;
