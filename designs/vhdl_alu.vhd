-- vhdl_alu.vhd -- First VHDL chip through Takahe
-- The US DoD would be proud. Probably. Hard to tell with them.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity vhdl_alu is
    port (
        a      : in  std_logic_vector(7 downto 0);
        b      : in  std_logic_vector(7 downto 0);
        op     : in  std_logic_vector(2 downto 0);
        result : out std_logic_vector(7 downto 0);
        zero   : out std_logic
    );
end entity vhdl_alu;

architecture rtl of vhdl_alu is
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
